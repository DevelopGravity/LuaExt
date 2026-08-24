# LuaExt

[![CI](https://img.shields.io/badge/CI-not_yet_configured-lightgrey)](#) <!-- TODO: point at .github/workflows/ci.yml once it reports status -->
[![Packagist](https://img.shields.io/badge/packagist-not_yet_published-lightgrey)](#) <!-- TODO: point at the Packagist page once developgravity/lua-ext is published -->
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

A PHP extension that embeds a vendored, patched **Lua 5.5.1** interpreter to run **untrusted, user-supplied Lua code** safely: portable CPU/wall-clock/memory limits, capability-based trust configuration, coroutines strictly scoped to a single call, a virtual filesystem the host implements in PHP, and host-controlled `require()`.

Package: `developgravity/lua-ext` · extension name `luaext` · namespace `DevelopGravity\LuaExt` · license MIT · PHP 8.5+.

> **Status: pre-1.0, no working build yet.** This repository does not have a tagged release, and only the build skeleton exists — no sandboxing behavior has been implemented or tested. The API surface below (classes, methods, parameters, defaults) is pinned and machine-validated against `stubs/luaext.stub.php` and `stubs/luaext_exceptions.stub.php`, the same files that generate the extension's C arginfo, so it will not drift further. What isn't yet true is the runtime: treat every code sample as accurate against that API, not as tested against a working binary.

## Why this exists

MediaWiki's `luasandbox` extension has three problems that this project exists to fix:

1. **Its CPU limit is a no-op outside Linux.** `setCPULimit()` is built on Linux-only POSIX timers. On macOS and Windows it silently compiles to a stub — the call succeeds, the limit is simply never enforced, and nothing tells you that. LuaExt's `Sandbox::features()` reports the real, per-platform enforcement level (`LimitSupport::Enforced` / `Degraded` / `Unsupported`) so a host can never be silently unprotected.
2. **It targets an old Lua.** `luasandbox` targets Lua 5.1; 5.4 support only just landed on its master branch, unreleased. LuaExt vendors and patches **Lua 5.5.1** directly — never the system `liblua` — so the sandboxing hooks live in the interpreter's hot loops instead of being bolted on from outside.
3. **It has no filesystem concept and no coroutines.** `luasandbox` removed coroutines entirely because its timeout hook couldn't span them. LuaExt exposes coroutines by default, capped and strictly scoped to the call that created them, and adds a host-implemented virtual filesystem (`FileSystem` interface) so scripts can do `io`-style work against storage the host controls.

This is a from-scratch rewrite, not a fork. There is no LuaSandbox compatibility shim — see [Migrating from LuaSandbox](#migrating-from-luasandbox) below for the mechanical rename most call sites need.

## Requirements

- PHP **8.5** or later (NTS and ZTS both supported, including FrankenPHP workers).
- Linux (x64, arm64) or macOS (x64, arm64): a C compiler toolchain to build from source. No system Lua is used or required.
- Windows x64: no toolchain needed — installs a prebuilt DLL. Windows on Arm runs the x64 build under emulation for v1; native arm64 is a fast-follow pending upstream `php-windows-builder` support.

## Install

Via [PIE](https://github.com/php/pie):

```bash
pie install developgravity/lua-ext
```

- **Linux / macOS**: PIE builds from source (`phpize && configure && make`) against the vendored Lua tree — nothing is downloaded or compiled outside this repository's `third_party/` sources.
- **Windows**: PIE fetches a prebuilt `php_luaext-{tag}-{php}-{ts|nts}-{vs}-x64.zip` from this repository's GitHub Releases instead of compiling. There is no Windows build toolchain requirement.

For IDE autocomplete and static analysis without loading the extension, add the stub package as a dev dependency once published:

```bash
composer require --dev developgravity/lua-ext-stubs
```

## Quick start

```php
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Sandbox;

$sandbox = new Sandbox();

$sandbox->registerLibrary('host', [
    'greet' => fn (string $name): string => "Hello, {$name}!",
]);

[$greeting] = $sandbox->eval('return host.greet("World")');

$doublerChunk = $sandbox->compile('return function(value) return value * 2 end', chunkName: 'double.lua');
[$doubleFunction] = $doublerChunk->call();
[$doubled] = $doubleFunction->call(21);

$sandbox->close();
```

Exposing an existing PHP object instead of a closure table uses `registerObject()` with the `#[LuaMethod]` attribute — only methods explicitly marked (or explicitly allowlisted) become callable from Lua:

```php
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\LuaMethod;
use DevelopGravity\LuaExt\Sandbox;

final class TextService
{
    #[LuaMethod]
    public function upper(string $inputText): string
    {
        return strtoupper($inputText);
    }
}

$sandbox = new Sandbox();
$sandbox->registerObject('text', new TextService());

[$shouted] = $sandbox->eval('return text.upper("hi there")');
```

A `new Sandbox()` with no arguments is the **untrusted baseline** — see below. Object identity never crosses the boundary: Lua only ever gets bound method callables, never a proxy it can introspect or mutate.

## Capabilities and limits

Trust is a single object, `Capabilities`, passed inside `SandboxConfig`. The default constructor (`new Capabilities()`) *is* the untrusted baseline — there is no separate "safe mode" flag to forget. `Capabilities::trusted()` is a preset that flips some flags; individual flags can always be overridden with `with()` regardless of preset.

| Capability | Untrusted default | `trusted()` | Notes |
|---|---|---|---|
| `loadBytecode` | `false` | `false` | Stays off even when trusted — see [SECURITY.md](SECURITY.md). Must be opted into explicitly with `with()`. |
| `compileAtRuntime` (Lua-visible `load()`) | `false` | `true` | |
| `dumpBytecode` | `false` | `true` | |
| `require` | `false` | `true` | |
| `vfs` / `vfsWrite` | `false` / `false` | `true` / `false` | Write access is a separate flag even when trusted. |
| `coroutines` | `true` | `true` | On by default in both presets; strictly call-scoped regardless. |
| `osTime` | `true` | `true` | |
| `osEnv` (+ allowlist) | `false` | `false` | |
| `debugTraceback` | `true` | `true` | |
| `debugIntrospect` | `false` | `true` | |
| `debugMutate` | `false` | `false` | Never flipped by a preset. |
| `debugHooks` | `false` | `false` | Defeats the watchdog by design; enabling it while a CPU **or** wall-clock limit is set throws `ConfigurationError` — both are delivered through the interpreter hook `debug.sethook` would displace. Never flipped by a preset. |
| `utf8` | `true` | `true` | |
| `gcControl` | `false` | `true` | |
| `warn` | `false` | `true` | |

`Limits` (defaults shown) caps resource use independent of trust level:

| Limit | Default |
|---|---|
| `memoryBytes` | 32 MiB |
| `cpuSeconds` | 1.0 |
| `wallClockSeconds` | 5.0 |
| `outputBytes` (+ `outputOverflow`) | 1 MiB, overflow `Fail` |
| `maxLiveCoroutines` | 64 |
| `maxCoroutineDepth` | 16 |
| `maxCallDepth` | 200 |
| `maxModules` | 64 |
| `maxRequireDepth` | 16 |
| `maxStringLength` | 64 MiB |
| `maxSourceBytes` | 1 MiB |
| `maxConversionDepth` | 64 |

Filesystem access has its own `VfsQuota` (open handles, file/total byte caps, operation counts, path length/depth) — see [docs/cookbook.md](docs/cookbook.md) for how it applies to a `FileSystem` backend.

## Feature support by platform

CPU-limit enforcement is portable in the sense that it never silently does nothing — but its *precision* is platform-dependent, and `Sandbox::features()` reports the honest number rather than a boolean. This is the direct answer to `luasandbox`'s silent no-op problem.

| Platform | Arch | Install | CPU clock source | Typical resolution | `features()->cpuLimit` |
|---|---|---|---|---|---|
| Linux | x64, arm64 | build from source | `pthread_getcpuclockid` + `clock_gettime` | ~nanoseconds | `Enforced` |
| macOS | x64, arm64 | build from source | `thread_info(THREAD_EXTENDED_INFO)` | ~microseconds | `Enforced` |
| Windows | x64 | prebuilt DLL | `GetThreadTimes` | **~15.6 ms** (scheduler tick) | `Degraded` |
| Windows | arm64 (WoA) | x64 DLL under emulation | `GetThreadTimes` | ~15.6 ms | `Degraded` |

On Windows, that ~15.6ms scheduler-tick resolution means short CPU limits can't be measured precisely. When `cpuSeconds` is set below roughly `4 × cpuResolutionSeconds`, the sandbox automatically arms a companion wall-clock deadline (`max(wallClockSeconds, cpuSeconds × 4 + 50ms)`) and reports `LimitSupport::Degraded`. A spinning script always dies on every platform; only timing *precision* degrades on Windows. Call `Sandbox::features()` at runtime rather than assuming a platform's behavior — it returns `cpuLimit`, `wallClockLimit`, `cpuResolutionSeconds`, `threadSafe`, and `platform`.

Native Windows arm64 (rather than x64-under-emulation) and further calibration work (e.g. `QueryThreadCycleTime`) are open items, not committed features — see the project plan's risk list.

## Migrating from LuaSandbox

There is no compatibility shim; call sites need a mechanical rename plus a couple of behavior changes. Names, parameters, and defaults below match `stubs/luaext.stub.php` and `stubs/luaext_exceptions.stub.php` exactly.

| LuaSandbox | LuaExt | Notes |
|---|---|---|
| `new LuaSandbox()` | `new Sandbox(new SandboxConfig(...))` | Trust and limits are passed explicitly as config objects instead of set post-construction. |
| `LuaSandbox::getVersionInfo()` | `Sandbox::extensionVersion()`, `Sandbox::luaVersion()` | Split into two static calls; add `Sandbox::features()` for limit-enforcement introspection. |
| `->loadString($code, $chunkName)` | `->compile($code, $chunkName): LuaFunction` | Same shape. |
| `->loadBinary($binary, $chunkName)` | `->compileBinary($binary, $chunkName): LuaFunction` | Now gated behind the `loadBytecode` capability, off by default even when trusted. |
| `->setMemoryLimit($bytes)` | `->setMemoryLimit($bytes)` | Unchanged name. VFS buffers and captured output are now billed against this limit too (they weren't in the old extension). |
| `->getMemoryUsage()` / `->getPeakMemoryUsage()` | `->getMemoryUsage()` / `->getPeakMemoryUsage()`, or `->stats()->memoryBytes` / `->stats()->peakMemoryBytes` | Unchanged names as direct getters; `stats(): SandboxStats` is the new full-snapshot surface (also has `memoryLimitBytes`, `outputBytes`, `outputTruncated`, `liveCoroutines`, `peakCoroutineDepth`, `modulesLoaded`, `vfsOperations`, `vfsBytes`, `gcCollections`, `luaCallsIn`, `phpCallsOut`). |
| `->setCPULimit($seconds)` | `->setCpuLimit($seconds)` | Renamed to camelCase; see the platform matrix above for what "enforced" means per OS. |
| `->getCPUUsage()` | `->getCpuUsage()`, or `->stats()->cpuSeconds` | Renamed to camelCase. |
| *(none)* | `->setWallClockLimit($seconds)` | New: an independent wall-clock ceiling, also the Windows CPU-limit backstop. |
| *(none)* | `->getWallClockUsage()`, or `->stats()->wallClockSeconds` | New: pairs with `setWallClockLimit()`. |
| `->pauseUsageTimer()` / `->unpauseUsageTimer()` | `->pauseTimers()` / `->resumeTimers()` | Renamed; same segment-accumulator semantics (only the outermost Lua entry arms/disarms). |
| `->enableProfiler($period)` / `->disableProfiler()` | `->enableProfiler($period)` / `->disableProfiler()` | Unchanged. |
| `->getProfilerFunctionReport($units)` with `LuaSandbox::SAMPLES/SECONDS/PERCENT` | `->getProfile(ProfilerUnit $unit = ProfilerUnit::Seconds): array` | Unit constants become the `ProfilerUnit` enum (`Samples`, `Seconds`, `Percent`); default unit is `Seconds`. |
| `->callFunction($name, ...$args)` | `->call($path, ...$args): array` | Both `call()` and `eval()` return `list<mixed>` and are `#[\NoDiscard]` — cast to `(void)` if you intentionally ignore the result. |
| `->wrapPhpFunction($callable)` | `->wrapCallable($callable, ?string $name = null): LuaFunction` | Renamed; optional `$name` labels the callable in tracebacks and `debug.getinfo()`. |
| `->registerLibrary($name, $functions)` | `->registerLibrary($name, $functions)` | Unchanged shape. |
| *(none)* | `->registerObject($name, $instance, ?array $methods = null)` | New: expose an existing object's methods via an allowlist or `#[LuaMethod]`. |
| *(none)* | `->preloadModule(...)`, `->interrupt()`, `->close()` | New: `require()` preloading, thread-safe host-triggered abort, and explicit/idempotent teardown. |
| `LuaSandboxFunction::call(...)` | `LuaFunction::call(...)` / `LuaFunction::__invoke(...)` | Same "array of all return values" convention. |
| `LuaSandboxFunction::dump()` | `LuaFunction::dump($strip)` | Now gated behind the `dumpBytecode` capability. |
| Host-side error → `false` return + `E_WARNING` | Host-side error → typed exception | E.g. calling an undefined global now throws rather than returning `false`. |
| `LuaSandboxError` / `...RuntimeError` / `...FatalError` / `...SyntaxError` / `...MemoryError` / `...ErrorError` / `...TimeoutError` | `LuaThrowable` interface, implemented by everything the extension throws; abstract `LuaException` → `RuntimeError` (catchable, plus subclasses `VfsError` and `ModuleNotFoundError`) and abstract `FatalError` (uncatchable) → `SyntaxError`, `MemoryLimitError`, `CpuLimitError`, `WallClockLimitError`, `OutputLimitError`, `CoroutineLimitError`, `HostAbortError`, `ErrorHandlerError`, `PanicError`, `ConversionError` | Roughly 1:1: `SyntaxError`←`...SyntaxError`, `MemoryLimitError`←`...MemoryError`, `ErrorHandlerError`←`...ErrorError`, `CpuLimitError`←`...TimeoutError`, plus new `WallClockLimitError` and others with no old equivalent. Host-misuse conditions (`ConfigurationError`, `CapabilityError`, `ClosedSandboxError`, `ThreadAffinityError`) extend a new abstract `LuaLogicException` (a `\LogicException`), not part of the old hierarchy at all. `LuaThrowable` also carries `getLuaTrace()`, `getLuaTraceAsString()`, `getSandbox()`, `getChunkName()`, and `getLuaLine()` — deliberately not `getLine()`, since PHP's `Exception::getLine()` is `final` and reports the PHP call site, not the Lua one. |
| *(coroutines removed entirely)* | Coroutines on by default, capped, strictly scoped to the call that created them | See [docs/lua-api.md](docs/lua-api.md#coroutines). |

## Documentation

- [SECURITY.md](SECURITY.md) — the threat model: what is and is not defended against, the trust model, and how to report a vulnerability.
- [CHANGELOG.md](CHANGELOG.md) — release notes (currently unreleased-only; no tags exist yet).
- [docs/cookbook.md](docs/cookbook.md) — practical recipes: host-service exposure (including a PDO/SQLite example), `FileSystem` backends, vendoring pure-Lua libraries, output capture, usage-based billing, coroutine patterns.
- [docs/lua-api.md](docs/lua-api.md) — the Lua-side reference: exactly which standard library members are available, replaced, or absent, plus `require()` and coroutine semantics.

## License

MIT. See [LICENSE](LICENSE). The vendored Lua interpreter under `third_party/lua-5.5.1/` keeps its own upstream MIT license file (`third_party/lua-5.5.1/LICENSE`) — Lua is Copyright © Lua.org, PUC-Rio, distributed under the same MIT terms as this project.
