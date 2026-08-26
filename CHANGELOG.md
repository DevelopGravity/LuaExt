# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/). Versioning is plain [semver](https://semver.org/) tags with **no `v` prefix** (e.g. `1.0.0`, not `v1.0.0`) — `composer.json` carries no version key; the git tag is the sole version record, and `PHP_LUAEXT_VERSION` in `php_luaext.h` must match it exactly.

There are no tagged releases yet.

## [Unreleased]

Nothing is released yet, so this section is a running record of what exists rather than a diff against a previous tag.

### Added

- **Vendored Lua 5.5.1**, patched. Every patch is guarded by `LUAEXT_LUA_HOOKS`, so compiling with it at `0` reproduces upstream byte for byte; `tools/vendor-lua.sh --check` verifies the tree against the pinned tarball plus the committed patch series.
- **Core execution**: `Sandbox` construction and teardown, `compile()`/`eval()`/`call()`, `LuaFunction` handles, dotted-path global access, and value conversion in both directions.
- **PHP callbacks**: `registerLibrary()`/`registerObject()`, including re-entrant Lua → PHP → Lua calls.
- **Runtime limits, actually enforced**: CPU, wall-clock, memory and output budgets. The interrupt check is compiled into the interpreter's own back edges rather than delivered through a debug hook, so it costs nothing on the dispatch path; a process-wide watchdog thread covers breaches while the owner is blocked outside the VM. `Sandbox::features()` reports the real per-platform enforcement level and clock resolution.
- **Capability-gated standard library**: an allow-list assembled member by member into a scratch table, never a scrubbed open state. Replacements for `print`, `pcall`/`xpcall`, `collectgarbage`, `math.randomseed` and `warn`; a filtered `debug` table; a hand-written `os` limited to time and environment. `tools/audit-stdlib.php` enforces the resulting surface against committed golden files.
- **Coroutines**, on by default and strictly call-scoped: every coroutine created during a call is force-closed when that call returns, so no suspended Lua state survives it. Capped by `maxLiveCoroutines` and `maxCoroutineDepth`, with a collection before the cap is enforced so the limit describes what is alive rather than what was ever created. The interrupt follows whichever coroutine is running, so work moved into one is still billed to the sandbox's CPU and memory budgets.
- **Fatal errors that user Lua cannot swallow**, covered by an adversarial suite spanning `pcall`, nested `pcall`, `xpcall`, `__gc` finalisers, `<close>` handlers, and — for both the CPU and the memory case — `resume`, `wrap`, nested `resume` and `resume` inside `xpcall`. The memory case is checked on the `lua_resume` status rather than the error value, because a refused allocation raises `LUA_ERRMEM` carrying Lua's own string instead of the extension's unforgeable marker.
- **`io` output half**: `io.write`, `io.stdout` and `io.stderr`, needing no capability because they touch no storage. Writing to `io.stderr` is what makes the output callback's `$isStderr` argument true; the sink flushes on a channel change so interleaved writes keep their order.
- **Virtual filesystem**: `io.open` with `:read`/`:write`/`:seek`/`:close`/`:flush`/`:lines`, plus `io.lines` and `os.remove`/`os.rename`, all behind the `vfs` capability and a host `FileSystem`. A backend implementing `RangedFileSystem` is streamed through `readRange`/`writeRange`; anything else is buffered whole-file, and a script cannot tell which it got. Every `VfsQuota` field is enforced, including the file count. Handles are call-scoped like coroutines: an unclosed file is flushed and closed when the call that opened it returns.
- **`require()`** behind its capability, resolving through `package.loaded`, a cycle guard, the two limits, `package.preload`, the filesystem along `modulePaths`, and finally a host `ModuleResolver`. `package` exposes only `loaded`, `preload` and a read-only `path` — no `cpath`, `searchers` or `loadlib`, because each of those exists to reach a shared object. A module that fails while loading is not cached, so a later `require()` retries rather than replaying the failure.
- **Sampling profiler**, off by default: `enableProfiler()`/`disableProfiler()`/`getProfile()`. Arming the count hook costs ~2.6x on dispatch-bound code, which is why the shipped hot path stays hook-free and this is opt-in. `enableProfiler()` returns `false` rather than displacing the count hook on a build where it is carrying the CPU limit.
- **`SandboxStats` reports measured figures**, including CPU and wall-clock time read off the watchdog and bytes moved through the filesystem. Those were hardcoded zeros behind a TODO.
- **Output sink** with buffer, callback and discard modes, billed against the memory limit.
- Project documentation: `README.md`, `SECURITY.md`, `docs/cookbook.md`, `docs/lua-api.md`.
- CI across Linux and macOS (x64 and arm64, NTS and ZTS, debug and release), plus valgrind, sanitizer and lint jobs.

### Known gaps

Every capability the extension defines is implemented, and `Sandbox::features()['capabilities']` reports so. What remains:

- The **Windows build does not compile yet**, and its CI job is non-gating.
- The **multi-threaded SAPI paths have no test coverage** — `.phpt` cannot spawn PHP threads.
