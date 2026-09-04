# Performance

What the sandbox costs to run Lua, measured against stock Lua 5.5.1.

> **Scope: the interpreter only.** No PHP boundary, no value conversion, no callback
> bridge, no watchdog thread. That is deliberate — it is the only figure comparable to
> a Lua author's own benchmark, and the only one honestly quotable as "what LuaExt
> costs to run Lua". A recipe that crosses into PHP on every iteration is dominated by
> the boundary, not by anything on this page.

## Method

`tools/bench-vm.sh` compiles `third_party/lua-5.5.1/src/` **three times from the same
files, the same compiler and the same flags**, differing only in two macros:

| Build | Macros | What it is |
|---|---|---|
| `stock` | `LUAEXT_LUA_HOOKS=0` | Upstream Lua 5.5.1, byte for byte — every patch is guarded by that macro |
| `shipped` | `LUAEXT_LUA_HOOKS=1` | What LuaExt actually ships: the interrupt check compiled into the four back edges a loop can close through |
| `hooked` | `LUAEXT_BENCH_HOOK=1` | The design this replaced: a `LUA_MASKCOUNT` hook whose body is **empty** |

Building the same tree three ways is the point. Timing a system `lua` binary instead
would measure that distribution's compiler flags at least as much as it measured us,
and would make any number here unquotable.

Timing is best-of-N — the minimum is the least noisy estimator available, since
scheduler interference and cache eviction can only ever make a run slower.

```bash
tools/bench-vm.sh 7        # best of 7; default is 5
```

## Results

**Apple M2 Max, 12 cores, arm64** · Apple clang 21.0.0 · `-O2` · best-of-7, two
independent runs, values averaged. Measured 2026-08-25 against `4878718`.

| Benchmark | Iterations | Stock | Shipped | Hooked | Shipped ns/iter | Shipped Δ | Hooked Δ | Run spread |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| arithmetic (while) | 20,000,000 | 82.7 ms | 86.7 ms | 227.4 ms | 4.33 | **+4.8%** | +175.0% | 1.2% |
| numeric for | 20,000,000 | 42.0 ms | 44.0 ms | 97.3 ms | 2.20 | **+4.8%** | +132.1% | 2.6% |
| generic for | 10,000,000 | 149.6 ms | 147.9 ms | 185.1 ms | 14.79 | −1.1% | +23.8% | 2.9% |
| tail recursion | 8,000,000 | 81.6 ms | 82.4 ms | 155.9 ms | 10.30 | +1.0% | +91.1% | 1.5% |
| function calls | 8,000,000 | 87.8 ms | 88.5 ms | 228.5 ms | 11.06 | +0.7% | +160.1% | 0.8% |
| table writes | 8,000,000 | 32.5 ms | 33.3 ms | 65.3 ms | 4.17 | +2.5% | +100.6% | 2.2% |
| string concat | 400,000 | 17.8 ms | 18.1 ms | 23.9 ms | 45.25 | +2.0% | +34.6% | 1.7% |
| pcall churn | 2,000,000 | 459.0 ms | 456.9 ms | 495.4 ms | 228.47 | −0.4% | +7.9% | 1.7% |
| **TOTAL** | **76,400,000** | **952.9 ms** | **957.8 ms** | **1478.9 ms** | | **+0.5%** | **+55.2%** | |

## Reading it honestly

**Run-to-run spread is 0.8–2.9%, which is larger than most of the per-benchmark
deltas.** Only the two tight arithmetic loops, at +4.8%, clear that noise floor. The
negative numbers for `generic for` and `pcall churn` are *not* the patched build being
faster — they are noise. The honest quotable range is:

> **0% to +5% on the interpreter, worst case ~+5%.**

That matches the mechanism. The check sits at four back edges — backward jumps,
numeric `for`, generic `for`, and tail calls — so its cost scales with **back edges per
unit of work**, not with wall time. A `while i < N` loop is almost nothing but back
edges, which is why it pays 4.8% at 4.33 ns/iteration. `pcall churn` spends 228 ns per
iteration inside call setup and error-handler machinery, so one extra, perfectly
predictable branch disappears into it.

## Why the third column exists

`hooked` is not a strawman — it is the design this one replaced, and its hook body here
is **empty**. It still costs **+55% overall and +175% worst case**.

The expense was never the body. Any non-zero `hookmask` sets `ci->u.l.trap`, and a set
trap makes `vmfetch` call `luaG_traceexec` on *every instruction*. Raising
`luaext.hook_count` does not help, because the count throttles the hook body, not the
per-instruction call that reaches it.

So the `lvm.c` patch buys roughly a **35× reduction in enforcement overhead**
(+0.5% against +55%). It is also why `luaext.hook_count=0` no longer voids the limits:
the interpreter carries the check itself, so there is no INI setting that can silently
disarm a security guarantee.

## What the profiler costs

`enableProfiler()` arms a `LUA_MASKCOUNT` hook — which is precisely the `hooked` build
above. That column is therefore not only a historical comparison; it is the price list
for sampling, and no separate benchmark is needed to state it.

| Workload shape | Cost with the profiler on |
|---|---:|
| tight arithmetic loop | 2.75× |
| function calls | 2.60× |
| numeric `for` | 2.32× |
| table writes | 2.01× |
| tail recursion | 1.91× |
| string concat | 1.35× |
| generic `for` | 1.24× |
| `pcall` churn | 1.08× |
| **whole suite** | **1.55×** |

> **Sampling costs ~2.6× on dispatch-bound code, up to 2.75× worst case, and ~1.55×
> across a mixed workload.**

The spread is the whole point, and it is not proportional to how much work a script
does. The cost is per *instruction dispatched*, because a non-zero `hookmask` sets
`ci->u.l.trap` and a set trap routes `vmfetch` through `luaG_traceexec` on every
instruction. So a loop doing almost nothing per iteration pays most, and `pcall` churn —
228 ns of call machinery per iteration — barely notices at 1.08×.

Two consequences worth stating plainly. Lowering the sampling period does **not** reduce
this: the period throttles the hook *body*, not the per-instruction call that reaches
it, so a rarely-firing profiler costs nearly what a busy one does. And this is exactly
why sampling is opt-in rather than always-armed — paying 2.6× on every call to collect a
profile nobody reads is the trade the shipped design refuses.

## The configuration matrix

Everything above answers one question very well: what the patched interpreter costs against
stock Lua. It says nothing about the rest of the configuration space, and that space has
grown — a compile cache, two shapes of filesystem backend, three output modes, an opt-in
profiler. A single headline per feature describes the default and nothing else.

`tools/bench-matrix.php` varies one axis at a time and prints the table below. It is
deliberately not a `.phpt` and does not run in CI, for the same reason the memory figures
below are not: a shared runner measures its neighbours as much as it measures this
extension.

```bash
php -d extension=modules/luaext.so tools/bench-matrix.php 7
```

**Apple M2 Max, arm64** · PHP 8.5.10 NTS · best of 7 · measured 2026-09-05.

### eval() compile cache

| Chunk | Cache off | Cache on | |
|---|---:|---:|---:|
| `return 1` (8 B) | 5.3 µs | 4.1 µs | 1.29× |
| small loop (42 B) | 6.6 µs | 4.4 µs | 1.51× |
| compute (~250 B) | 34.9 µs | 29.9 µs | 1.16× |
| 60 functions (3.5 KB) | 51.4 µs | 12.9 µs | **3.99×** |

**The gain tracks how much of the call is parsing.** A chunk that does real work amortises
its own parse — `compute` is 1.16× because most of its 30 µs is execution the cache cannot
touch. A large chunk that returns quickly is almost entirely parse, and gains most.

### Profiler

| Workload | Off | On | |
|---|---:|---:|---:|
| tight `while` loop | 28.8 µs | 66.6 µs | **2.31×** |
| function calls | 27.9 µs | 64.2 µs | **2.30×** |
| mixed compute | 27.5 µs | 36.9 µs | 1.34× |

Consistent with the `hooked` column above, and for the same mechanism: the cost is per
instruction *dispatched*, so dispatch-bound code pays most.

### Output mode

500 `io.write` calls per iteration.

| Mode | Per call | |
|---|---:|---:|
| `Buffer` | 56.7 µs | 1.00× |
| `Discard` | 50.8 µs | 0.90× |
| `Callback` | 159.1 µs | **2.81×** |

**`Callback` costs roughly 3×**, and that is the boundary rather than the sink: every chunk
crosses into PHP and back. It buys streaming — output leaves the sandbox as it is produced
instead of accumulating against `Limits::$outputBytes` — so the trade is latency and memory
against throughput. `Discard` is the floor, and the gap between it and `Buffer` is what
retaining the bytes costs.

### Filesystem backend shape

40 writes plus a full read-back per iteration.

| Backend | Per call | |
|---|---:|---:|
| `FileSystem` (buffered) | 26.7 µs | 1.00× |
| `RangedFileSystem` (streamed) | 626.0 µs | **23.40×** |

**This is the largest single choice in the matrix, and the direction surprises people.**
A buffered `FileSystem` is read whole at open and written whole at close: 40 writes cost
*one* call into PHP. A `RangedFileSystem` streams, so each write is its own `writeRange()`
— 40 boundary crossings instead of one, and each one is a full `zend_call_known_instance_method`.

So `RangedFileSystem` is not the faster interface; it is the **scalable** one. It exists so
a script can work with a file larger than `Limits::$memoryBytes`, which the buffered path
cannot do at all, and so a write is durable when it happens rather than at close. If your
files are small and your scripts write in many small pieces, the buffered backend is both
simpler and far quicker. Implement `RangedFileSystem` when the file size demands it, not by
default.

### Sandbox lifecycle

| Step | Cost |
|---|---:|
| construct + close, no script | 32.9 µs |
| construct + close, `vfs` granted | 36.0 µs |
| construct + eval + close | 82.9 µs |

**A per-request sandbox pays ~33 µs before it runs anything**, and no setting reduces it —
it is a fresh `lua_State`, its standard library, and the capability filtering. Granting the
VFS adds ~3 µs. This is the figure that decides whether the compile cache can help you at
all: a sandbox built, evaluated once and closed never sees a warm cache.

## What the eval() compile cache saves, and when it saves nothing

`eval()` parses its source on every call and discards the chunk.
`SandboxConfig(cacheCompiledChunks: true)` keeps it instead, bounded by
`Limits::$maxCachedChunks` (default 64).

The measured figures are in [the matrix above](#evalcompile-cache); this section is the
part a table cannot say.

> **~4× on a chunk of real size — but only for a sandbox that outlives several
> evaluations of the same source.**

An earlier measurement of this quoted 5.6×, against a differently-shaped 3.5 KB chunk on
an older build. Both are true of what they measured, which is the reason the matrix names
its workloads: the ratio is a property of the chunk, not of the cache.

**The second half of that sentence matters as much as the first.** A sandbox built per
request, evaluated once, and closed compiles into an empty cache every time and gains
**nothing**. Measured on this machine, that shape costs ~128 µs end to end: ~33 µs to
construct the interpreter and ~79 µs to parse. The cache cannot touch either. If that is
your pattern, this setting is pure overhead and should stay off.

It is also why the gain is single-digit rather than the ~10× that comparing `eval()`
against `compile()` + `call()` suggests: the cache key is the chunk name and the source, so every
call still copies and hashes the whole source to look it up. That cost scales with the
script, and it is the price of a lookup that cannot collide.

The cached chunks are ordinary Lua objects allocated through the sandbox's own allocator,
so they count against `memoryBytes` and appear in `stats()->cachedChunks`. That is the
reason the feature is off by default: switching it on moves a sandbox closer to its own
ceiling, which is not a thing to do to a caller who never asked for it.

## Memory across many sandboxes

A long-lived worker creates and closes sandboxes indefinitely, so "does a create/close
cycle leave anything behind" is a different question from anything above.

**Apple M2 Max, arm64** · PHP 8.5.9 NTS · macOS 26.6.2 · measured 2026-08-27 against
`91a9c2f`, with `leaks --atExit` under `MallocStackLogging=1`. Each cycle constructs a
sandbox, evaluates a small table-building script, and closes it.

```
1,600 cycles   →   0 leaks for 0 total leaked bytes
10,000 cycles  →   RSS +560 KiB, PHP heap flat at 2,048 KiB throughout
```

Nothing is unreachable, and the RSS figure is allocator arena growth rather than
accumulation. The per-1,000-cycle deltas say so directly:

```
+0, +144, +16, +16, +0, +32, +0, +32, +288, +32 KiB
```

Three rounds grew by nothing at all, and a genuine per-cycle leak cannot skip rounds. At
the observed 0.056 KiB/cycle average a real leak would have cost ~5.6 MiB over the same
run; the actual total is a tenth of that and flat in the middle. For reference, a PHP
process with no extension loaded grows ~176 KiB on its own over a comparable loop.

> **A create/close cycle leaks nothing.** Worker-mode use does not accumulate.

This is deliberately not a `.phpt`. An RSS assertion on a shared CI runner measures the
runner's allocator and its neighbours as much as it measures this extension, and the
project has twice declined to write that test.

## Caveats

- **arm64 and clang only.** The `lvm.c` patch has never been compiled by GCC on
  x86-64, where `LUA_USE_JUMPTABLE` may place `dojump` in computed-goto dispatch rather
  than the switch measured here. Re-run this on that target before quoting these
  numbers for Linux x64.
- **Release flags.** `-O2`, matching a release build. A `-O0` comparison would flatter
  the patched build by drowning it in dispatch cost.
- **Not a throughput benchmark for the extension.** Anything involving
  `registerLibrary` callbacks or value conversion is a different measurement that this
  file does not make. Sandbox construction and profiling *are* covered, in the two
  sections above.
- **The memory figures are macOS-only.** `leaks` has no Linux equivalent used here; the
  valgrind CI leg covers that platform on a much shorter run, and the plateau shape
  above has not been reproduced under a different allocator.
