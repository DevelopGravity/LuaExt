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

## Caveats

- **arm64 and clang only.** The `lvm.c` patch has never been compiled by GCC on
  x86-64, where `LUA_USE_JUMPTABLE` may place `dojump` in computed-goto dispatch rather
  than the switch measured here. Re-run this on that target before quoting these
  numbers for Linux x64.
- **Release flags.** `-O2`, matching a release build. A `-O0` comparison would flatter
  the patched build by drowning it in dispatch cost.
- **Not a throughput benchmark for the extension.** Anything involving
  `registerLibrary` callbacks, value conversion, or sandbox construction is a different
  measurement that this file does not make.
