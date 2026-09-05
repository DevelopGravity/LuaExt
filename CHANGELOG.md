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
- Project documentation: `README.md`, `SECURITY.md`, and `docs/` — configuration,
  cookbook, exceptions, the Lua-side API, migration, performance and platform support.
- CI across Linux and macOS (x64 and arm64, NTS and ZTS, debug and release), plus valgrind, sanitizer and lint jobs.
- **Lua language conformance suite** (`tests/10-lua/`): roughly 400 assertions over the language itself — arithmetic and the integer/float split, metatables, the pattern matcher, `string.pack`, the table and math libraries, `utf8`, coroutines, error semantics, `<close>`, and the replaced `io`/`os`. The rest of the suite proves the sandbox holds and that the expected stdlib *names* exist; this proves the patched interpreter still computes Lua's answers. It is platform-neutral by construction and is the first part of the suite Windows runs.
- **`make check` and `make dev`**: one command that runs every gate CI runs, and one that runs the build, the suite and the gates together. A gate that cannot run reports `SKIPPED` and exits non-zero, because a check that did not run is not a check that passed.
- **`CONTRIBUTING.md`**: how to build (including the debug PHP, which is the only build whose allocator reports leaks), what each gate catches, where a new test belongs, and the C conventions — chiefly that `lua_error()` longjmps, so a frame holding an allocation across anything that can raise leaks it.

### Changed

- **`Sandbox`'s usage and limit surface is smaller.** Six getters — `getMemoryUsage()`, `getPeakMemoryUsage()`, `getCpuUsage()`, `getWallClockUsage()`, `getOutputLength()`, `isOutputTruncated()` — are **removed**; each was a `stats()` field under another name, and `stats(): SandboxStats` is now the one way to read usage. The three limit setters are **replaced by `setLimits(Limits)`**, with `limits(): Limits` to read them back: the old three reached three of the fourteen limits a `Limits` carries, and the other eleven were always changeable but had no door. `$sandbox->setLimits($sandbox->limits()->with(cpuSeconds: 2.0))` changes one field. This also ends an inconsistency where `cpuSeconds: 0.0` meant "no limit" through the constructor and was refused by the setter. See [docs/migrating-from-luasandbox.md](docs/migrating-from-luasandbox.md).

### Fixed

Nothing has been tagged, so none of these ever shipped — but each is recorded because each came with a regression test and, where the mistake was mechanically detectable, a new rule that refuses it.

- **`VfsQuota::$maxOperations` was never reset.** Documented and implemented as a budget *per sandbox call*, it was in practice per sandbox *lifetime*: the function whose only job is resetting the counter was written, declared, and called from nowhere. A long-lived sandbox eventually refused every filesystem call it was asked for.
- **Three leaks on the paths that only run when something has already gone wrong.** `io.open`, `os.remove` and `os.rename` leaked their canonical path on every quota refusal, and a ranged `file:write()` leaked its whole payload — script-sized, so a loop writing 1 MB chunks leaked a megabyte per refusal. All four are the same cause: `lua_error()` longjmps past the frame's cleanup. Ownership now sits with Lua's collector.
- **`io.lines` read its path after freeing it** — a use-after-free on every "no such file".
- **A CPU breach close to the end of a call could go unreported.** The watchdog thread sets the interrupt flag when it wakes at the deadline, but the return boundary read only the flag and then disarmed the slot — so a script that crossed its deadline just before returning was back in PHP before the thread's wakeup, which then found nothing to service. On an idle machine 60 of 200 near-deadline breaches returned success; on the shared macOS CI runners the loss was systematic. The boundary now samples the deadline directly, once, before reading the flag, making the verdict independent of thread scheduling.
- **A write refused by the memory budget was reported as an output-limit breach.** The sink returned one `false` for both refusals, so a host whose `memoryBytes` filled while buffering output was told "The sandbox has written all the output it is allowed" — with `outputBytes` barely touched. The refusal now names the budget that actually ran out and raises `MemoryLimitError` for it.
- **`make install` was hidden, and a bare `make` discarded configuration.** The `GNUmakefile` shadow that scopes `clean` also concealed every generated target — `pie install` failed with *no rule to make target 'install'* — and its `build` target re-ran `./configure` over whatever flags a caller had configured with, so CI's sanitizer legs silently compiled against the wrong PHP and every debug leg silently built release. The shadow now forwards unknown targets and only configures a tree that has no Makefile yet, and CI asserts both.
- **`coroutine.resume` answered `message, false`** instead of `false, message` when a coroutine resumed itself: `lua_xmove` is a no-op when its source and destination are the same stack, so the `false` landed on top of the message rather than beneath it.
- **`coroutine.close` accepted a running or normal coroutine**, resetting the stack it was executing on and returning `true`. Upstream refuses both; now so does this.
- **`io.lines` and `file:lines` ignored their format argument.** `f:lines("L")` silently dropped the newline it was specifically asked to keep — no error, just the wrong bytes. Both now route through the same code `:read()` uses, and accept multiple formats and byte counts as Lua does.
- **The PHP 8.5 floor was unenforced at build time.** Declared in `composer.json` and honoured by PIE, but a plain `phpize && make` against 8.4 failed somewhere deep in a translation unit rather than saying so.

### Known gaps

Every capability the extension defines is implemented, and `Sandbox::features()['capabilities']` reports so. What remains:

- The **Windows build compiles and gates**, but runs only `tests/10-lua/` — the
  platform-neutral conformance suite. `tests/02-limits/` asserts far below Windows'
  ~15.6 ms scheduler tick and needs guards written against observed failures rather
  than guessed at.
- The **multi-threaded SAPI paths have no test coverage** — `.phpt` cannot spawn PHP threads.
