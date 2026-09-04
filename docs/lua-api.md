# Lua API reference

This is the reference for what a Lua script actually sees inside a `Sandbox` — which standard library members exist, which are LuaExt's own replacements and why, which upstream members are simply absent, how `io`/`os` map onto the host-controlled virtual filesystem, `require()` semantics, and coroutine behavior.

> **Status: pre-1.0, no tagged release.** The stdlib policy described here is **implemented and tested**: library exposure is assembled by LuaExt's own `luaext_openlibs.c`, which copies approved members out of a scratch table rather than scrubbing a fully-open Lua state, and `tools/audit-stdlib.php` enforces the resulting surface against committed golden files on every push.
>
> For the exact surface a default sandbox exposes, run `tools/audit-stdlib.php` — the golden files it checks are the authoritative answer, and this page is prose written to match them.

Everything here applies per-sandbox, gated by that sandbox's `Capabilities`. Two presets are referenced throughout: **Untrusted** (`new Capabilities()`, the default) and **Trusted** (`Capabilities::trusted()`). See [SECURITY.md](../SECURITY.md) for the full trust model and the reasoning behind each restriction.

## Library-by-library

| Library | Untrusted | Trusted |
|---|---|---|
| `base` (globals) | Filtered, with replacements — see below | Adds `load` (text-only; `mode = "b"` additionally requires the `loadBytecode` capability, which stays off even here) |
| `coroutine` | LuaExt's own wrapper around upstream, gated by the `coroutines` capability (on by default), capped and call-scoped — see [Coroutines](#coroutines) | Same |
| `string` | Open; `string.dump` removed; `string.format("%p")` rejected | `string.dump` restored behind the `dumpBytecode` capability |
| `table` | **All members**, including `table.move` and `table.create` (their loops are patched to be interruptible) | Same |
| `math` | Open; `math.randomseed` replaced | Same |
| `utf8` | Open, with an interruptible scan | Same |
| `os` | LuaExt's own, **not** upstream's: `clock`, `date`, `difftime`, `time` under `osTime`; `getenv` under `osEnv` + allowlist | Same |
| `io` | LuaExt's own, **not** upstream's. The output half (`io.write`, `io.stdout`, `io.stderr`) is unconditional; the filesystem half (`io.open`, `io.lines`, handles) needs `vfs` — see [io/os emulation](#ioos-emulation) | Same |
| `package` | Upstream version **never linked into the binary**. LuaExt's replacement appears with the `require` capability and carries only `loaded`, `preload` and a read-only `path` — no `cpath`, `searchers` or `loadlib` | Same |
| `debug` | `debug.traceback` only | Adds `debug.getinfo`/`getlocal`/`getupvalue` behind `debugIntrospect`; `debug.sethook` only via the separate `debugHooks` capability (mutually exclusive with a CPU limit — see SECURITY.md) |

## Replaced members, and why

- **`tostring`** no longer falls through to `lua_topointer` for tables/functions/userdata — printing a value never leaks its heap address.
- **`print`** writes to the sandbox's configured output sink (`OutputMode::Buffer`/`Callback`/`Discard`) instead of a process-wide stream; there is no stdout to write to inside the sandbox at all.
- **`pcall` / `xpcall`** catch `RuntimeError`-family (catchable) errors exactly like upstream, but re-raise `FatalError`-family errors (a CPU/wall-clock/memory/output limit trip, a host abort, a coroutine limit, …) instead of returning `false, err`. For `xpcall`, the message handler is not invoked at all when the underlying error is fatal — a script cannot inspect, log, or otherwise interact with a fatal error from inside its own handler.
- **`collectgarbage`** is restricted to `count`, `step`, `isrunning` and `collect` for untrusted scripts — `collect` is included deliberately, since collecting *more* is the safe direction; the tuning verbs are withheld because a script could otherwise defeat the sandbox's own allocator-level GC-pressure tuning. `gcControl` (granted under `Trusted`) restores the tuning verbs.
- **`math.randomseed()`** no longer returns anything. Upstream Lua 5.4+ returns the seed components it derived, which are partly address-based — an information leak useful for defeating ASLR. LuaExt's replacement is `void`, takes only integer input, and when called with no arguments seeds from the sandbox's configured seed source (a CSPRNG unless the host explicitly opted into a fixed, deterministic seed).
- **`os.clock()`** (LuaExt's own `os`, not upstream's) is sandbox-local rather than process-wide, and its resolution is intentionally rounded to roughly 20 microseconds to avoid becoming a high-resolution timing side channel.
- **`warn`** (Lua 5.4+'s warning system) is gated behind the `warn` capability and, when enabled, routes to the same output sink rather than the process's stderr; `lua_setwarnf` is always installed by the extension so that nothing from an unconfigured warning system reaches the host process's stderr by default.
- **`lua_newstate`**'s hash-seed is supplied explicitly from the extension's own CSPRNG (or a host-provided fixed seed under `deterministic: true`) rather than Lua's own `luaL_makeseed`, which has the same address-derived-entropy property being avoided elsewhere.

## Absent entirely

These are not filtered or wrapped — they simply do not exist in a LuaExt sandbox, because the source files that implement them (`liolib.c`, `loslib.c`, `loadlib.c`, `linit.c`, `lua.c`, `luac.c`) are excluded from the build entirely. This is verified, not assumed: the linked binary carries zero references to `system()`, `tmpnam()`, `popen()`, or `setlocale()` — the code that could call them was never compiled in.

- Upstream **`io`**, **`os`**, and **`package`** as shipped by stock Lua. All three are present only as LuaExt's own hand-written tables, never as restricted views of upstream's — which is why `package` carries no `cpath`, `searchers` or `loadlib` to restrict in the first place.
- **`io.read`** and **`io.stdin`**, at every trust level and regardless of capability. A sandbox has no console to read from, and a stub that always returned `nil` would be a surface that looks like it might one day do something.
- **`load`**, **`loadfile`**, **`dofile`** for untrusted scripts. `load` returns under `Trusted` (text mode only, per the table above); `loadfile`/`dofile` are not reintroduced under any preset — module loading goes through `require()` instead (see below).
- **`string.dump`** for untrusted scripts (returns under `Trusted` behind `dumpBytecode`).
- **`string.format("%p")`** — rejected as an error at every trust level; it cannot be selectively filtered the way a whole library member can, since the format string itself is otherwise a normal, needed feature.

## io/os emulation

> **The `io` table has two halves, and they answer to different things.**
>
> The **output half** — `io.write`, `io.stdout`, `io.stderr` — needs no capability at all. Writing a partial line is not a storage operation, and requiring `vfs` for it would mean a script could not write without also being handed a filesystem backend. These go to the same sink as `print()`; `io.stderr` is what makes the output callback's `$isStderr` parameter true.
>
> The **filesystem half** — `io.open`, file handles, `io.lines` — needs the `vfs` capability and a `FileSystem` behind it. Writing additionally needs `vfsWrite`.

Every sandbox has an `io` table carrying the output half: `io.write` (which accepts strings and numbers and returns the table so writes chain), plus `io.stdout` and `io.stderr`, each with `:write` and a `:close` that politely refuses the way upstream's does for a standard stream. Nothing here touches storage.

With the `vfs` capability granted, that same table additionally gains the filesystem half — `io.open`, file-handle `:read`/`:write`/`:seek`, `io.lines` — with every operation routed through a C layer (path canonicalization, handle bookkeeping, quota enforcement) to a PHP `FileSystem` implementation the host supplies. Without `vfs`, those names are simply absent from the table rather than present-and-failing, so a script can test for `io.open` to discover whether it has a filesystem.

- **Path model**: a pure, virtual POSIX-style namespace rooted at `/`. `.`/`..` are resolved lexically; a path that would escape the root is an *error*, not silently clamped back to `/`. Backslash is an ordinary filename character, never a separator. NUL bytes, overlength paths, and excessively deep nesting are rejected before a backend ever sees them.
- **Quotas** (`VfsQuota`, independent of `Limits`): open-handle count, per-file and total byte caps, file count, operation count, path length/depth. **`maxOperations` is spent per sandbox CALL and refilled for the next one** — it bounds how much host work one call may demand, not how much a sandbox may do in its lifetime. See [docs/cookbook.md](cookbook.md) for how these interact with a real backend.
- **Read formats**: `"l"`, `"L"`, `"a"` and a byte count all behave as Lua's do, on both `:read()` and `lines()`. **`"n"` is refused**, deliberately — upstream parses a number off the stream, which means a C scanner reading an unbounded run of digits out of host-controlled data. Read bytes and use `tonumber()`.
- **Errors**: not-found, permission, and quota failures come back the conventional Lua way — `nil, message, code` — script-handleable like any other `io` error. If the backend itself throws something other than a `VfsError` (a Redis connection failure, say), that's treated as fatal and the original PHP exception is preserved and rethrown to the host; a backend outage is not something a script is expected to `pcall` around.
- **`os` replacement**: `os.clock` is sandbox-local and rounded (see above); `os.date`/`os.time`/`os.difftime` are gated behind the `osTime` capability (on by default); `os.remove`/`os.rename` route through the same VFS layer as `io`. There is no `os.execute`, `os.exit`, or `os.tmpname` — not merely hidden from scripts, but structurally absent, since `loslib.c` (the upstream implementation of all three) is excluded from the build entirely (see [Absent entirely](#absent-entirely)). Nor is there an unconditional `os.getenv` — it only exists behind the `osEnv` capability, and only for an explicitly configured allowlist of names.
- **`package` replacement**: exposes only `package.loaded` and `package.preload` (both writable the way upstream's are) and a read-only `package.path`. There is no `package.cpath`, `package.searchers`, or `package.loadlib` — nothing in this table can reach outside the sandbox.

## `require()` semantics

`require()` only exists at all when the `require` capability is granted (off by default in both presets — `Trusted` turns it on). When available, resolution for a module name matching `[A-Za-z0-9_.-]+` (128 characters max, no `..` segments) proceeds in order:

1. `package.loaded` — already-resolved modules, keyed by name.
2. A circular-require guard (a module that `require`s itself, directly or transitively, fails cleanly rather than recursing).
3. `Limits::maxModules` (total distinct modules a sandbox may load) and `Limits::maxRequireDepth` (nesting depth of `require` calls) are checked.
4. `package.preload` — PHP-registered loader functions (`Sandbox::preloadModule(...)`).
5. The VFS, searched along `SandboxConfig::modulePaths` (default `['/?.lua', '/?/init.lua']`) — this is the mechanism for vendoring pure-Lua libraries; see the cookbook.
6. A PHP `ModuleResolver`, as a final fallback.

Resolved source compiles via `luaL_loadbufferx` in text mode (`"t"`) unless the `loadBytecode` capability additionally allows a resolver to hand back bytecode. A module that fails during loading is **not** cached in `package.loaded` — a subsequent `require()` of the same name gets a fresh attempt, not a cached failure.

Pure-Lua third-party libraries — including pure-Lua LuaRocks packages such as `dkjson`, `penlight`, or `inspect` — work by vendoring their source into the VFS (or a `ModuleResolver`) and letting `require()` find them there; they execute fully inside the sandbox, under the same CPU/memory/coroutine limits as the rest of the script. Binary (C) LuaRocks cannot be loaded under any configuration — see [SECURITY.md](../SECURITY.md#what-this-does-not-defend-against) for why that's an architectural property, not a missing feature.

## Coroutines

Coroutines are available by default (`coroutines` capability defaults to `true` even in the Untrusted preset) through LuaExt's own `coroutine` table — a thin wrapper around upstream's `create`/`resume`/`yield`/`status`/`running`/`isyieldable`/`wrap`, not a restricted subset of it.

- **Caps**: `Limits::maxLiveCoroutines` (default 64) bounds how many coroutines can exist at once — `coroutine.create` runs a GC step to reclaim dead coroutines before actually failing the cap. `Limits::maxCoroutineDepth` (default 16) bounds nested `resume` depth.
- **Resource accounting spans coroutines.** CPU and memory limits are sandbox-wide, not per-coroutine — a script cannot dodge a CPU limit by moving work into a coroutine. `coroutine.resume` publishes the currently-running Lua state to the watchdog so an interrupt always lands on whichever coroutine is actually executing, however deeply nested.
- **Fatal errors are not swallowable through coroutines either.** Because `resume` behaves like a `pcall` internally, both `coroutine.resume` and `coroutine.wrap` re-raise fatal errors (a tripped limit, for instance) rather than returning them as an ordinary `false, err` coroutine failure.
- **Strictly call-scoped lifecycle — the core guarantee.** No suspended Lua execution state survives past the PHP call that created it. When the outermost `Sandbox::call()`/`eval()` returns — however it returns, including via an error or a timeout — every coroutine created during that call is force-closed (`<close>` variables run as part of the close). If a script stashed a coroutine reference in a global table, that reference survives only as a **dead thread value**; resuming it in a later call raises Lua's ordinary, catchable "cannot resume dead coroutine" error, not a crash or a resurrection of old state. After any call returns, a sandbox holds only plain data — nil, booleans, numbers, strings, tables, and functions — never a suspended coroutine.
- **Hard limitation: no yielding across the PHP boundary.** `coroutine.yield` called from inside a PHP callback (Lua calls a registered PHP function, which internally is still "inside" that C call, and something downstream tries to yield) raises Lua's standard "attempt to yield across a C-call boundary" error. PHP call frames cannot be suspended and resumed the way Lua frames can. Patterns that need to pause mid-callback have to restructure around this — see [docs/cookbook.md](cookbook.md#coroutine-patterns) for iterator/generator shapes that work within it.
- **No PHP-side coroutine handles.** A Lua coroutine (thread) value passed as an argument to a PHP callback, or returned from `Sandbox::call()`, converts to `ConversionError` — there is no PHP object representing a live Lua coroutine. Coroutines are an in-script control-flow tool only; anything that needs to look like background work from PHP's perspective belongs on the PHP side (amphp, ReactPHP, Fibers), not inside the Lua state. See the cookbook for the reasoning and for what patterns do fit.

## Values crossing the PHP boundary

A few conversion rules matter to anyone writing Lua that's meant to interoperate closely with `registerLibrary`/`registerObject` calls or with values returned to PHP:

- Numbers preserve Lua's own int/float distinction (`lua_isinteger`) rather than guessing from the value — there's no risk of a whole-number float silently becoming a PHP int or vice versa.
- Integer table keys use the full 64-bit range when converting from PHP; a Lua table with both a numeric key and its string form (e.g. `t[1]` and `t["1"]`) is a `ConversionError`, not a silent collision, in either direction.
- Circular references (a table that contains itself, directly or transitively) are a `ConversionError` that reports the cycle's path, not a hang or a truncated copy. Conversion recursion is capped at depth 64.
- Lua functions convert to/from `LuaFunction` objects on the PHP side. Lua threads (coroutines) do not convert at all — see above.
