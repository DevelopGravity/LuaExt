# Security

LuaExt's entire purpose is running Lua code you do not trust. This document is the threat model: what the sandbox is designed to defend against, what it explicitly does not, the trust model that governs both, how to report a vulnerability, and the policy that keeps found issues from regressing.

> **Status: pre-1.0, no tagged release, and no external audit.** Two of the three defenses described here are implemented and covered by tests: the **watchdog** (CPU, wall-clock, memory and output budgets, enforced from inside the interpreter's own dispatch loop) and the **stdlib policy** (an allow-list assembled member by member, enforced against committed golden files on every push). The adversarial suite covers a script trying to catch its own limit breach through `pcall`, nested `pcall`, `xpcall`, a `__gc` finaliser and a `<close>` handler.
>
> The **VFS is not built** — `io` carries only its output half (`io.write`, `io.stdout`, `io.stderr`, which touch no storage), there is no `io.open`, and no `FileSystem` is ever consulted. Every VFS claim below is design, not defense.
>
> One coverage gap is worth stating plainly rather than burying: the **multi-threaded SAPI paths have no test coverage** — `.phpt` cannot spawn PHP threads, and the sanitizer legs build NTS php-src, so the watchdog has never been exercised against more than one PHP thread. See [what this does not defend against](#what-this-does-not-defend-against).
>
> No part of this has been reviewed by anyone outside the project. Treat it as a design specification with a growing amount of test evidence behind it, not as an audited, hardened binary. The [security regression policy](#security-regression-policy) below is the mechanism intended to keep that honest.

## What this sandbox is designed to defend against

**Untrusted-script CPU exhaustion.** A process-wide watchdog thread tracks per-sandbox thread-CPU time against `Limits::cpuSeconds` and raises a sticky interrupt flag when it's exceeded. Delivery is in four tiers:

1. **The interpreter itself.** The vendored `lvm.c` checks the flag at the four back edges a loop can close through — backward jumps, numeric `for`, generic `for`, and tail calls. This is the primary mechanism and it costs no hook: any non-zero `hookmask` sets `ci->u.l.trap`, which makes `vmfetch` call `luaG_traceexec` on *every* instruction, measured at 2.6× on dispatch-bound code.
2. **`LUAEXT_CHECK` in vendored Lua's long-running C loops** — pattern matching, `table.move`/`sort`/`concat`, UTF-8 scanning. This is what makes `table.move(t, 1, 2^40, 1, {})` and catastrophic backtracking interruptible; the older `luasandbox` banned `table.move` outright because it had no equivalent.
3. **The PHP call boundary.**
4. **A count hook, only as a fallback** when the watchdog thread could not be started — where its strided clock self-check becomes the only thing that could notice an overrun.

A cross-thread `lua_sethook` "accelerator" appeared in earlier drafts of this design and was **removed as a data race**: `lua_sethook` writes `L->hook`/`hookmask` non-atomically and then walks the `CallInfo` chain writing `ci->u.l.trap`, while the owning thread pushes and pops `CallInfo`s on every call. Upstream's "safe from a signal handler" note means *same-thread* safety, not cross-thread.

The flag is deliberately **sticky** — nothing clears it on raise. That is what stops a `__gc` finaliser continuing after a breach, since Lua's own `GCTM` runs finalisers under a protected call and downgrades any error to a warning.

**Untrusted-script memory exhaustion.** All Lua heap allocation goes through a custom `lua_Alloc` that enforces `Limits::memoryBytes`. Critically, VFS read/write buffers and captured script output are PHP-side memory that would otherwise be invisible to that allocator — a script could exhaust host RAM through `io.write` or a large file read without ever touching the Lua heap. LuaExt bills both explicitly against the same memory limit via a charge/discharge API, closing that hole.

**Output flooding.** `Limits::outputBytes` caps total captured/streamed output. Overflow behavior is a config choice — `Truncate` keeps going with a flag set, `Fail` raises an *uncatchable* `OutputLimitError`. The `Fail` case matters because it cannot be swallowed by `pcall`/`xpcall` — a script can't buy itself unlimited output by wrapping its own writes in a handler.

**Filesystem escape.** The virtual filesystem is a pure, fuzzed path-normalization module rooted at `/`: NUL bytes, overlength paths, and excessive nesting depth are rejected outright, `.`/`..` are resolved lexically, and escaping the virtual root is an *error*, not a silent clamp back to `/`. Backslash is treated as an ordinary filename character, not a path separator, so Windows-style traversal tricks don't get special treatment. Backends always receive a canonical absolute path.

**Address / ASLR disclosure.** Several stock Lua behaviors leak process addresses to script code, which is a real primitive for defeating ASLR in an attack chain. LuaExt replaces or removes all of them: `tostring` no longer calls through to `lua_topointer`; `string.format("%p")` is rejected outright (it can't be filtered by policy since the format string itself is otherwise essential); `math.randomseed()` — which upstream Lua 5.4+ returns as address-derived seed components back to the calling script — is replaced with a void, integer-only version seeded from configuration or a CSPRNG; and the Lua state itself is seeded via `lua_newstate`'s explicit seed parameter from a CSPRNG rather than `luaL_makeseed`, which has the same address-entropy property.

**Stdlib escape vectors.** `io`, `os`, and `package` are never linked into the binary as upstream ships them — not filtered post-hoc, simply absent, and replaced by a VFS-backed `io`/`os` emulation and a host-controlled `require()`. `loadlib.c`, `liolib.c`, `loslib.c`, `linit.c`, `lua.c`, and `luac.c` are excluded from the build entirely, and the vendored Lua is compiled in strict ISO C mode (no `LUA_USE_POSIX`/`LINUX`/`MACOSX`/`DLOPEN`), so `dlopen`/`popen`/`system`/`tmpnam`/`setlocale` are unreachable even at link time — confirmed, not assumed: the linked binary carries zero references to `system()`, `tmpnam()`, `popen()`, or `setlocale()`, because the code that could call them was never compiled in. This is also why binary (C) LuaRocks cannot be loaded, by architecture rather than by policy (see [what this does not defend against](#what-this-does-not-defend-against)). The `debug` library is `traceback`-only for untrusted code; `getinfo`/`getlocal`/`getupvalue` and `sethook` require explicit capabilities. `collectgarbage` is restricted to `count`/`step`/`isrunning` for untrusted code — the tuning verbs are withheld because they would let a script defeat the allocator's own GC-pressure tuning. `string.dump` is removed unless `dumpBytecode` is granted.

**Timeout-swallowing via `pcall`/`xpcall`/`coroutine.resume`.** Every exception the extension raises is either catchable or not by construction: `RuntimeError` (and its subclasses) can be caught inside Lua; the abstract `FatalError` hierarchy (`CpuLimitError`, `WallClockLimitError`, `MemoryLimitError`, `OutputLimitError`, and others) cannot be, at the language level. LuaExt's own `pcall`/`xpcall` replacements re-raise fatal errors instead of returning `false, err` — for `xpcall` specifically, the message handler is skipped entirely when the error is fatal, so a script can't inspect or suppress a timeout from inside its own handler. `coroutine.resume`/`coroutine.wrap` follow the same rule, since `resume` is effectively a `pcall` in disguise: ours re-raise fatals through every level of nested resume.

One case in that family deserves naming, because passing it does not follow from passing the others. Every fatal LuaExt raises travels as an unforgeable userdata **except** a refused allocation: Lua raises `LUA_ERRMEM` carrying its own preallocated string, since building our marker would itself need to allocate. A wrapper that inspected the error *value* would therefore stop a CPU, wall-clock or output breach and still let a script move its allocation into a coroutine and resume past `memoryBytes`. The check keys on the **status** from `lua_resume`, with `LUA_ERRMEM` named explicitly, and the status is converted back into the fatal marker rather than re-raised as a string — re-raising would leave the enclosing protected call seeing `LUA_ERRRUN`, so a nested `pcall` would catch what the resume refused.

Nine reachable escapes are covered and gating: `pcall`, nested `pcall`, `xpcall`, `__gc`, `<close>`, and — for both the CPU and the memory case — `resume`, `wrap`, nested `resume`, and `resume` inside `xpcall`.

## What this does not defend against

**Bytecode loading is unsafe even when enabled.** `compileBinary()`/`load(..., "b")` are gated behind the `loadBytecode` capability, and that capability stays **off even under `Capabilities::trusted()`** — it requires an explicit `with(loadBytecode: true)` override. This is deliberate: the Lua 5.5 reference manual itself states that loading malicious or malformed bytecode can crash the interpreter or worse, because the bytecode loader does not fully re-validate what it's given the way the text compiler does. Enabling `loadBytecode` moves bytecode integrity into the host's own threat model — LuaExt cannot make untrusted bytecode safe, only text source.

**`debugHooks` defeats the watchdog, by design.** A host granted the `debugHooks` capability could install a competing Lua debug hook. Rather than try to make the two coexist, the extension refuses the combination outright: enabling `debugHooks` while **either** a CPU or a wall-clock limit is set throws `ConfigurationError` at configuration time. Both are delivered through the same mechanism `debug.sethook` would displace, so refusing only the CPU case would leave the wall limit quietly defeated. Since `Limits` carries non-zero defaults for both, this means `debugHooks` is refused unless a host explicitly clears them. If you need both, you don't get both — pick one.

**A buggy host `FileSystem` backend can still betray the sandbox.** The VFS layer canonicalizes and validates every path in C before it ever reaches the host's `FileSystem` implementation, but it cannot audit what that implementation actually *does* with a canonical path — a backend that concatenates it onto a host filesystem root incorrectly, leaks data across sandboxes, or otherwise misbehaves is outside the extension's control. There's a subtler gap worth calling out explicitly: by default (`VfsQuota::billWallTime = false`) the wall-clock deadline is *paused* for the duration of a backend call, and CPU time is never billed for it either (the host, not the Lua interpreter, is doing the work). A backend that hangs — a stalled network call to Redis, for instance — will not trip `WallClockLimitError` or `CpuLimitError` on its own; the call simply blocks until the backend returns, subject only to whatever timeout the backend itself enforces (or none). Hosts using a slow or externally-dependent `FileSystem` backend should set `billWallTime: true` or otherwise bound backend latency themselves.

**Binary LuaRocks are unsupported by architecture, not by choice.** This is a capability gap, not a vulnerability, but it's worth being explicit: because the vendored Lua is compiled with hidden symbol visibility and exports no `lua_*` symbols for a `dlopen`'d module to resolve against, and because `loadlib.c` isn't even compiled in, C-extension LuaRocks packages cannot be loaded at all — sandboxed or not. Native functionality (SQLite, HTTP, etc.) has to come from a host service exposed via `registerObject()`/`registerLibrary()` instead; see [docs/cookbook.md](docs/cookbook.md) for the pattern.

**Fixed PRNG/hash seeds require an explicit opt-in.** `SandboxConfig`'s `seed` defaults to `null`, meaning the Lua state is seeded from a CSPRNG — this is what defeats hash-flood denial-of-service attacks against Lua's table implementation. Passing a fixed `seed` is supported (for reproducible test fixtures, for example) but requires also passing `deterministic: true`, or the extension throws `ConfigurationError`. This exists so that surrendering hash-flood protection is always a conscious decision, never an accidental default.

**Thread affinity is a correctness requirement, not an attacker-facing boundary.** A `Sandbox` is pinned to the OS thread that created it; every method except `interrupt()` throws `ThreadAffinityError` if called from another thread. This exists to keep per-thread CPU clocks meaningful and to avoid a class of FrankenPHP worker-mode bugs — it is not itself a defense against a malicious script, since the script never controls which thread calls into it.

**Coroutines cannot suspend across the PHP boundary.** `coroutine.yield` called from inside a PHP callback trampoline (Lua → PHP function → attempted yield) raises Lua's own "attempt to yield across a C-call boundary" error. This is a documented hard limitation of the embedding, not a vulnerability, but it means coroutine-based patterns that assume they can yield through an arbitrary call stack will not work here — see [docs/cookbook.md](docs/cookbook.md) for patterns that do.

**The multi-threaded SAPI paths are untested.** The watchdog is designed for one process-wide thread servicing sandboxes owned by many PHP threads — FrankenPHP and other worker SAPIs run several per process — and its purity rules exist for exactly that: the slot holds only a `luaext_irq *`, and `luaext_watchdog.c` includes neither `php.h` nor `lua.h`, so it cannot reach a `zend_object`, module globals, or a TSRM context even by accident. A CI job enforces that structurally.

What has **no test coverage** is the behaviour under real concurrency. `.phpt` cannot spawn PHP threads, and the sanitizer legs build NTS php-src, so pool contention, cross-thread slot recycling, and the MSHUTDOWN join have never been exercised against more than one PHP thread. The design is reasoned; it is not demonstrated. Hosts running a threaded SAPI should treat this as the least-proven part of the extension, and should build a sandbox per request rather than caching one across worker threads — a `Sandbox` picked up by a different thread throws `ThreadAffinityError` by design.

**Handing a sandbox to its own script defeats every limit it has.** This is the one failure mode where the host, not the script, is the vulnerability — and it is not guarded against.

Every limit in this extension is enforced in C, below anything a script can reach. But `Sandbox` is an ordinary PHP object, and the callback bridge will expose any callable a host registers. So both of these hand a script the controls:

```php
$sandbox->registerObject('box', $sandbox);              // direct
$sandbox->registerLibrary('u', [                        // indirect, via capture
    'go' => fn () => $sandbox->setCpuLimit(3600.0),
]);
```

A script holding either can call `setCpuLimit()` and `setMemoryLimit()` to raise its own ceilings, `takeOutput()` in a loop to keep resetting the output budget (see [docs/cookbook.md](docs/cookbook.md) on what that limit bounds), `interrupt()` to abort a sibling call, or `close()` to destroy the interpreter mid-flight. None of that is an escape from the sandbox in the memory-safety sense; it is the host having granted authority it did not mean to grant.

**There is deliberately no check for this, and the reason is that a partial one would be worse than none.** An `instanceof Sandbox` guard in `registerObject()` is bypassed in one line by wrapping the call in a closure, which `registerLibrary()` accepts and must accept — closures over host state are the entire point of the callback bridge. Shipping that guard would let a host believe the case was handled while leaving the one-line bypass wide open. A guarantee that looks real and is not is a worse security property than a documented gap.

The rule is short: **never let the sandbox object, or anything closing over it, cross into the script.** Expose services, not the machinery that bounds them.

## Trust model

Trust is expressed as a single `Capabilities` value object, not a global setting or an implicit default tied to configuration elsewhere:

- **`new Capabilities()` is the untrusted baseline.** Every flag defaults closed except `coroutines`, `osTime`, `debugTraceback`, and `utf8`. There is no separate "safe mode" toggle to remember to set — the constructor's defaults *are* the safe mode.
- **`Capabilities::trusted()`** is a preset, not a different code path: it's a static constructor that flips `compileAtRuntime`, `dumpBytecode`, `require`, `vfs`, `debugIntrospect`, `gcControl`, and `warn` to `true`. It deliberately does **not** flip `loadBytecode`, `debugMutate`, or `debugHooks` — those stay closed under every preset and require an explicit `with(...)` override, because each has an unconditional cost described above (unsafe-even-when-enabled bytecode, watchdog defeat).
- **`with(...$overrides)`** clone-with semantics apply on top of either preset, so a host can start from `trusted()` and still withhold, say, `vfsWrite`, or start from the untrusted default and grant only `coroutines: false` for a script that must not use them.

Limits (`Limits`, `VfsQuota`) are configured independently of trust — a trusted script still runs under a memory ceiling and CPU budget unless the host explicitly raises or removes them.

## Reporting a vulnerability

<!-- TODO: replace with a real, monitored security contact before this project accepts external users. -->
**TODO:** Security contact has not been established yet. Once it is, this section should specify a private reporting channel (e.g. a security@ address or GitHub private vulnerability reporting) and be linked from `SECURITY.md`'s standard location so GitHub surfaces it automatically.

<!-- TODO: define and commit to a real response-time SLA once there is a maintainer team able to honor one. -->
**TODO:** Expected response time has not been established yet.

Please do not open a public issue for a suspected vulnerability. Until the contact above is filled in, treat any of the "what this is designed to defend against" mechanisms above as in scope for private disclosure the moment a maintainer contact exists.

## Security regression policy

- `tests/03-adversarial/` is **append-only**. Every security finding — whether from internal review, a sanitizer or fuzzer crash, or an external report through the process above — lands as a permanent `.phpt` reproducing the attack *before* its fix is merged. A fix without a corresponding adversarial test that would have caught it is not considered complete.
- Sanitizer legs (ASan, UBSan, and a separate TSan run specifically targeting the watchdog and `tests/02-limits/`) are **merge-gating**, not advisory. A PR that introduces a sanitizer finding does not merge.
- Every release is gated on a 24-hour fuzz run (path normalizer and PHP↔Lua conversion targets) plus the full adversarial suite passing on every supported platform.
- The stdlib surface exposed under each `Capabilities` preset is checked against a committed golden-file expectation (`tools/audit-stdlib.php`); any new member introduced by a future Lua point release that isn't already in the expectation file fails CI until a human reviews and explicitly approves it. This exists specifically because of lessons like `table.move`, which the old extension had to ban outright for lack of a patched, interruptible implementation — new stdlib surface is not free to expose by default.
