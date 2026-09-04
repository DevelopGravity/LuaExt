# LuaExt

[![CI](https://github.com/DevelopGravity/LuaExt/actions/workflows/ci.yml/badge.svg?branch=develop)](https://github.com/DevelopGravity/LuaExt/actions/workflows/ci.yml)
[![Lint](https://github.com/DevelopGravity/LuaExt/actions/workflows/lint.yml/badge.svg?branch=develop)](https://github.com/DevelopGravity/LuaExt/actions/workflows/lint.yml)
[![Packagist](https://img.shields.io/packagist/v/developgravity/lua-ext?include_prereleases&label=packagist)](https://packagist.org/packages/developgravity/lua-ext)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

A PHP extension that embeds a vendored, patched **Lua 5.5.1** interpreter to run **untrusted, user-supplied Lua code** safely: portable CPU/wall-clock/memory limits enforced inside the interpreter itself, and capability-based trust configuration.

Package: `developgravity/lua-ext` · extension name `luaext` · namespace `DevelopGravity\LuaExt` · license MIT · PHP 8.5+.

> **Status: pre-1.0, no tagged release, no external audit.** Every capability the extension defines is implemented, and 137 tests cover compilation, the PHP↔Lua boundary, the CPU/wall-clock/memory/output budgets, the capability-gated standard library, coroutines, the virtual filesystem, `require()`, the profiler, Lua language conformance, and the adversarial cases where a script tries to catch its own limit breach.

## Why this exists

MediaWiki's `luasandbox` has three problems this project exists to fix.

1. **Its CPU limit is a no-op outside Linux.** `setCPULimit()` is built on Linux-only POSIX timers; on macOS and Windows it silently compiles to a stub. The call succeeds, the limit is never enforced, and nothing tells you. `Sandbox::features()` reports the real, per-platform enforcement level, so a host can never be silently unprotected — see [platform support](docs/platform-support.md).
2. **It targets an old Lua.** `luasandbox` targets Lua 5.1. LuaExt vendors and patches **Lua 5.5.1** directly — never the system `liblua` — so the sandboxing checks live in the interpreter's hot loops instead of being bolted on from outside, which is why they cost [0–5% rather than +55%](docs/performance.md).
3. **It has no filesystem concept and no coroutines.** `luasandbox` removed coroutines entirely because its timeout hook could not span them. LuaExt exposes them by default, capped and strictly call-scoped — the interrupt follows whichever coroutine is running, so work cannot be hidden in one. It also adds a host-implemented virtual filesystem so scripts can do `io`-style work against storage the host controls, with every path canonicalised and every quota enforced before a backend is called.

This is a from-scratch rewrite, not a fork, and there is no compatibility shim — see [migrating from LuaSandbox](docs/migrating-from-luasandbox.md).

## Requirements

- PHP **8.5** or later (NTS and ZTS, including FrankenPHP workers). The build refuses anything older.
- **Linux** (x64, arm64) or **macOS** (x64, arm64): a C toolchain to build from source. No system Lua is used or required.
- **Windows** x64: builds, and installs as a prebuilt DLL with no toolchain needed. See [platform support](docs/platform-support.md) for what "enforced" means per OS and for the current state of Windows test coverage.

## Install

Via [PIE](https://github.com/php/pie):

```bash
pie install developgravity/lua-ext:dev-develop
```

**The version is not optional yet.** The package is on Packagist but has no tagged release, so `dev-develop` is the only version that resolves — and it tracks the branch tip. Pin a commit (`dev-develop#<sha>`) if you need reproducibility before the first tag. Building from a checkout (`phpize && ./configure && make`) works too and is what CI exercises.

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

Exposing an existing object uses `registerObject()` with the `#[LuaMethod]` attribute — only methods explicitly marked (or explicitly allowlisted) become callable from Lua:

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

**`new Sandbox()` with no arguments is the untrusted baseline** — every capability closed except coroutines, `os.time`, `debug.traceback` and `utf8`, and every limit at its default. There is no separate "safe mode" flag to forget. Object identity never crosses the boundary: Lua only ever gets bound method callables, never a proxy it can introspect or mutate.

## Documentation

**Using it**

- [docs/configuration.md](docs/configuration.md) — `Capabilities`, `Limits`, `VfsQuota`: every field, its default, and what it bounds.
- [docs/cookbook.md](docs/cookbook.md) — practical recipes: host services (including a PDO/SQLite example), `FileSystem` backends, vendoring pure-Lua libraries, output capture, usage-based billing, coroutine patterns.
- [docs/lua-api.md](docs/lua-api.md) — the Lua-side reference: exactly which standard library members are available, replaced or absent, plus `require()` and coroutine semantics.
- [docs/exceptions.md](docs/exceptions.md) — the exception hierarchy, what a script can and cannot catch, and how to read a Lua traceback.

**Choosing how to run it**

- [docs/platform-support.md](docs/platform-support.md) — per-platform CPU-clock precision and what `features()` reports.
- [docs/performance.md](docs/performance.md) — what the sandbox costs, measured: the interpreter against stock Lua, and a matrix across the compile cache, output modes, filesystem backend shapes and the profiler.
- [docs/migrating-from-luasandbox.md](docs/migrating-from-luasandbox.md) — the mechanical rename, and the behaviour changes that are not mechanical.

**Trusting it**

- [SECURITY.md](SECURITY.md) — the threat model: what is and is not defended against, the trust model, and how to report a vulnerability.
- [CHANGELOG.md](CHANGELOG.md) — release notes (currently unreleased-only; no tags exist yet).

## License

MIT. See [LICENSE](LICENSE). The vendored Lua interpreter under `third_party/lua-5.5.1/` keeps its own upstream MIT license file — Lua is Copyright © Lua.org, PUC-Rio, distributed under the same MIT terms as this project.
