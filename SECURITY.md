# Security

LuaExt's entire purpose is running Lua code you do not trust. This document is the threat model: what the sandbox is designed to defend against, what it explicitly does not, the trust model that governs both, how to report a vulnerability, and the policy that keeps found issues from regressing.

> **Status: pre-1.0, no tagged release, and no external audit.** Two of the three defenses described here are implemented and covered by tests: the **watchdog** (CPU, wall-clock, memory and output budgets, enforced from inside the interpreter's own dispatch loop) and the **stdlib policy** (an allow-list assembled member by member, enforced against committed golden files on every push). The adversarial suite covers a script trying to catch its own limit breach through `pcall`, nested `pcall`, `xpcall`, a `__gc` finaliser and a `<close>` handler.
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

**A script can label its own error, but cannot change it.** An error the extension raises is an unforgeable userdata: a script cannot read it, write it, reach its metatable (`getmetatable` returns `false`), replace its metatable, or construct one — Lua has no constructor for userdata — so it cannot turn a catchable error into a fatal or the reverse. What it *can* choose, given the `compileAtRuntime` capability, is what its error is **labelled**: `load(code, chunkname)` names the chunk, so `getChunkName()` and the `source` field of a frame can say whatever the script picked. This is inherent to Lua — that argument means the same thing in every embedding — and it is attribution spoofing rather than an escape: nothing leaves the sandbox and no capability is gained. **`getLuaTraceAsString()` is the attributable one**, because the full traceback still carries the real calling frame beneath whatever the script named. A host that logs `getChunkName()` alone, and treats it as provenance, is trusting a value the script chose.

**Exception serialization drops the sandbox, and validates on the way back.** `LuaException` and everything under it implement `__serialize`/`__unserialize` so a failure can survive a queue. The sandbox reference is deliberately not carried — it wraps a live `lua_State`, and `getSandbox()` returns null on the far side rather than a revived object that is not the same one. PHP's own `getTrace()`/`getPrevious()` are dropped for the same reason: both can capture arbitrary live objects. Coming back in, the traceback is validated field by field against the shape the capture path produces and rejected whole if it does not match, because `unserialize()` writes into an object without a constructor and is the one route by which a payload could otherwise hand an exception a Lua context it never had. That matters only to a host that unserializes untrusted data — which has already lost more than a traceback — but the handler is the boundary, so it treats its input as hostile.

**Bytecode loading is unsafe even when enabled, so it is gated twice.** Lua's binary loader checks a chunk's header, buffer bounds, constant tags and string indices — and then stops. It does not validate opcodes, register indices, constant indices or jump targets, so corruption in the instruction stream reaches the VM intact. Measured by flipping one byte at each position and loading each: on a small chunk **57% were refused, 23% ran and returned the right answer anyway, 8% ran and returned a WRONG one, and 13% killed the process**. It degrades with size, because the validated header is a shrinking fraction of the blob — on a 297 KB chunk only **17% were refused and 82% loaded and ran**. A *crafted* chunk is arbitrary native execution, not a parse error. There is no verifier to add — Lua has never had one.

So there are two gates, and both must be open:

- The **`loadBytecode` capability**, which stays off even under `Capabilities::trusted()` and needs an explicit `with(loadBytecode: true)`.
- The **`luaext.allow_raw_bytecode` INI setting, off by default**, which governs *unsealed* blobs for both doors into the loader: `compileBinary()` and a script's own `load($bytes, name, "b")`. Gating only the host side would leave a script granted `loadBytecode` able to assemble bytes itself and reach the same loader.

**Sealed bytecode is the supported path and needs no INI change.** Everything `dump()` produces is sealed, and `compileBinary()` verifies it before the loader sees a byte. There are two modes, chosen with `SandboxConfig::$sealMode`:

- **`SealMode::Checksum`** (the default) seals with an unkeyed xxh128. It is tamper-*evident*: 128 bits catch corruption essentially always, at ~12 GB/s — 25 µs on a 297 KB blob against 1219 µs for HMAC. It needs no key, and it stops nobody deliberate, because anyone can recompute it.
- **`SealMode::Authenticated`** seals with HMAC-SHA256 over `SandboxConfig::$bytecodeKey` (≥16 bytes, from `random_bytes(32)`), compared in constant time. It adds what a checksum cannot: a blob sealed under one key will not load under another, so a bytecode store shared between processes fails **closed** rather than silently working.

A blob is always verified against the mode **the sandbox is configured for**, never the one the blob announces — otherwise an authenticated chunk could be downgraded by re-sealing it with the unkeyed checksum anybody can compute. Mode and key must agree at construction: a key without `Authenticated`, or `Authenticated` without a key, is refused rather than quietly resolved. Every single-byte corruption and every truncation of a sealed blob is refused, in both modes.

**Sealing is never a permission for a script.** The script-side `load($bytes, name, "b")` does not consult a seal and must not start: the default seal is unkeyed, so a script could compute one out of `string.char()`. That path is gated by the capability *and* the INI, unconditionally.

**What sealing does not buy.** It authenticates *origin*, not *safety*: it closes accidental corruption and tampering by anyone without the key, and it makes "never share the bytecode store" enforced rather than advised, since a blob sealed under one key will not load under another. It does **not** survive host compromise — an attacker who can read process memory has the key — and a host that stores the key beside the bytecode has authenticated nothing. LuaExt still cannot make untrusted bytecode safe, only text source.

**`debugHooks` defeats the watchdog, by design.** A host granted the `debugHooks` capability could install a competing Lua debug hook. Rather than try to make the two coexist, the extension refuses the combination outright: enabling `debugHooks` while **either** a CPU or a wall-clock limit is set throws `ConfigurationError` at configuration time. Both are delivered through the same mechanism `debug.sethook` would displace, so refusing only the CPU case would leave the wall limit quietly defeated. Since `Limits` carries non-zero defaults for both, this means `debugHooks` is refused unless a host explicitly clears them. If you need both, you don't get both — pick one.

**A buggy host `FileSystem` backend can still betray the sandbox.** The VFS layer canonicalizes and validates every path in C before it ever reaches the host's `FileSystem` implementation, but it cannot audit what that implementation actually *does* with a canonical path — a backend that concatenates it onto a host filesystem root incorrectly, leaks data across sandboxes, or otherwise misbehaves is outside the extension's control. There's a subtler gap worth calling out explicitly: by default (`VfsQuota::billWallTime = false`) the wall-clock deadline is *paused* for the duration of a backend call, and CPU time is never billed for it either (the host, not the Lua interpreter, is doing the work). A backend that hangs — a stalled network call to Redis, for instance — will not trip `WallClockLimitError` or `CpuLimitError` on its own; the call simply blocks until the backend returns, subject only to whatever timeout the backend itself enforces (or none). Hosts using a slow or externally-dependent `FileSystem` backend should set `billWallTime: true` or otherwise bound backend latency themselves.

**Timing limits do not pre-empt host callbacks, by design.** A callable registered through `registerLibrary()`/`registerObject()` (and the output callback) is the host's own trusted code, and the sandbox never interrupts it mid-flight — killing trusted code between two statements could abandon a half-committed transaction or a held lock, which is a worse outcome than the delay. The time *is* billed against `cpuSeconds`/`wallClockSeconds` and the breach is delivered the instant the callback returns, so the overshoot is bounded by one callback's duration — but a callback that blocks for ten seconds delays enforcement by ten seconds. Only the host can bound that safely: put timeouts on your own I/O. See [docs/configuration.md](docs/configuration.md#where-the-timing-limits-are-actually-checked) for the measured behavior.

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
    'go' => fn () => $sandbox->setLimits($sandbox->limits()->with(cpuSeconds: 3600.0)),
]);
```

A script holding either can call `setLimits()` to raise every one of its own ceilings at once, `takeOutput()` in a loop to keep resetting the output budget (see [docs/cookbook.md](docs/cookbook.md) on what that limit bounds), `interrupt()` to abort a sibling call, or `close()` to destroy the interpreter mid-flight. None of that is an escape from the sandbox in the memory-safety sense; it is the host having granted authority it did not mean to grant.

**There is deliberately no check for this, and the reason is that a partial one would be worse than none.** An `instanceof Sandbox` guard in `registerObject()` is bypassed in one line by wrapping the call in a closure, which `registerLibrary()` accepts and must accept — closures over host state are the entire point of the callback bridge. Shipping that guard would let a host believe the case was handled while leaving the one-line bypass wide open. A guarantee that looks real and is not is a worse security property than a documented gap.

The rule is short: **never let the sandbox object, or anything closing over it, cross into the script.** Expose services, not the machinery that bounds them.

## Trust model

Trust is expressed as a single `Capabilities` value object, not a global setting or an implicit default tied to configuration elsewhere:

- **`new Capabilities()` is the untrusted baseline.** Every flag defaults closed except `coroutines`, `osTime`, `debugTraceback`, and `utf8`. There is no separate "safe mode" toggle to remember to set — the constructor's defaults *are* the safe mode.
- **`Capabilities::trusted()`** is a preset, not a different code path: it's a static constructor that flips `compileAtRuntime`, `dumpBytecode`, `require`, `vfs`, `debugIntrospect`, `gcControl`, and `warn` to `true`. It deliberately does **not** flip `loadBytecode`, `debugMutate`, or `debugHooks` — those stay closed under every preset and require an explicit `with(...)` override, because each has an unconditional cost described above (unsafe-even-when-enabled bytecode, watchdog defeat).
- **`with(...$overrides)`** clone-with semantics apply on top of either preset, so a host can start from `trusted()` and still withhold, say, `vfsWrite`, or start from the untrusted default and grant only `coroutines: false` for a script that must not use them.

Limits (`Limits`, `VfsQuota`) are configured independently of trust — a trusted script still runs under a memory ceiling and CPU budget unless the host explicitly raises or removes them.

## Reporting a vulnerability

**Two private channels. Please do not open a public issue.**

- **GitHub** — [private vulnerability reporting](https://github.com/DevelopGravity/LuaExt/security/advisories/new) is enabled on this repository (Security → Report a vulnerability). Preferred: it threads the conversation, tracks whether it has been answered, and produces an advisory when it is fixed.
- **Email** — <security@developgravity.com>, if you would rather not use GitHub.

Encrypted reports are welcome. The key for that address is committed to this repository as [`security-disclosure.asc`](security-disclosure.asc), so you can encrypt without trusting a keyserver lookup:

```
Develop Gravity LLC - Security Disclosure <security@developgravity.com>
ed25519, created 2026-01-28
D3AE BBF0 BA59 370F 4CE7  BFB3 C916 7C4A 7A3A 671A
```

Verify it against the fingerprint above before use — a key file in a repository is only as trustworthy as the repository, and the fingerprint is what a second channel can confirm.

**What to include.** The version or commit, the platform, and something that reproduces it — a `.phpt`, a script, or a description precise enough to write one. A reproduction is worth more than a severity rating; the classification is our job and the reproduction is the part only you have.

### Best effort, and no bounty

LuaExt is **open source, maintained by Develop Gravity LLC on a best-effort basis**. It is not a commercial product, there is no support contract behind it, and nobody is paid to be on call for it. That is the honest frame for everything in this section.

Two things follow from it, and both are better said than inferred:

- **No reward, bounty, or payment is offered for any report**, and none will be. If you are looking for paid work, this is not that. Credit in the changelog and in the fix's commit is offered gladly to anyone who wants it.
- **Response is best effort**, in the ordinary sense of the phrase: reports are taken seriously and acted on, but by people doing it around other work rather than to a contracted schedule.

### Scope

**In scope:** everything under [what this sandbox is designed to defend against](#what-this-sandbox-is-designed-to-defend-against) — a script escaping its CPU, wall-clock, memory or output budget; reaching the host filesystem; leaking a process address; loading bytecode past both gates; swallowing a fatal through `pcall`, a coroutine, a finaliser or a `<close>` handler. Anything under `src/`, including the patches this project applies to the vendored interpreter, is ours.

**Not in scope, and please report it upstream instead:** a bug in the Lua language or its interpreter that reproduces against **stock Lua 5.5.1**. Those belong to [the Lua maintainers](https://www.lua.org/bugs.html), who fix them for everyone rather than for this one embedding; a fix landing there reaches us through `tools/check-lua-upstream.sh` and the vendoring script. The distinction is a practical one, and `tools/bench-vm.sh` builds a stock tree if you need to check which side of it you are on — if stock Lua does it too, it is Lua's; if only this build does, it is ours and we want to hear.

Also out of scope: the failure modes documented under [what this does not defend against](#what-this-does-not-defend-against). Those are known, deliberate and written down — most of all handing a sandbox to its own script, which is a host mistake the extension states plainly that it does not guard.

### What to expect

This is a **pre-1.0 project with no external audit**, maintained on a best-effort basis. Rather than promise a response time nobody is staffed to honour: reports are read as soon as they are seen, and you will get an acknowledgement telling you whether it is being worked on. If a report goes unanswered for two weeks, assume it was missed and send it again — that is a likelier explanation than it being ignored. A GitHub advisory is the surer of the two channels for exactly that reason: it shows you its own state.

Disclosure timing is yours to set; say what you want in the report. Absent anything else, the intent is to fix first and publish the fix with the reproduction, because the adversarial suite below is append-only and a finding that lands there stays covered forever.

## Security regression policy

- `tests/03-adversarial/` is **append-only**. Every security finding — whether from internal review, a sanitizer or fuzzer crash, or an external report through the process above — lands as a permanent `.phpt` reproducing the attack *before* its fix is merged. A fix without a corresponding adversarial test that would have caught it is not considered complete.
- Sanitizer legs (ASan, UBSan, and a separate TSan run specifically targeting the watchdog and `tests/02-limits/`) are **merge-gating**, not advisory. A PR that introduces a sanitizer finding does not merge.
- Every release is gated on a 24-hour fuzz run (path normalizer and PHP↔Lua conversion targets) plus the full adversarial suite passing on every supported platform.
- The stdlib surface exposed under each `Capabilities` preset is checked against a committed golden-file expectation (`tools/audit-stdlib.php`); any new member introduced by a future Lua point release that isn't already in the expectation file fails CI until a human reviews and explicitly approves it. This exists specifically because of lessons like `table.move`, which the old extension had to ban outright for lack of a patched, interruptible implementation — new stdlib surface is not free to expose by default.
