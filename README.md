# LuaExt

[![CI](https://github.com/DevelopGravity/LuaExt/actions/workflows/ci.yml/badge.svg?branch=develop)](https://github.com/DevelopGravity/LuaExt/actions/workflows/ci.yml)
[![Lint](https://github.com/DevelopGravity/LuaExt/actions/workflows/lint.yml/badge.svg?branch=develop)](https://github.com/DevelopGravity/LuaExt/actions/workflows/lint.yml)
[![Packagist](https://img.shields.io/packagist/v/developgravity/lua-ext?include_prereleases&label=packagist)](https://packagist.org/packages/developgravity/lua-ext)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

A PHP extension that embeds a vendored, patched **Lua 5.5.1** interpreter to run **untrusted, user-supplied Lua code** safely: portable CPU/wall-clock/memory limits enforced inside the interpreter itself, and capability-based trust configuration.

Package: `developgravity/lua-ext` · extension name `luaext` · namespace `DevelopGravity\LuaExt` · license MIT · PHP 8.5+.

> **Status: pre-1.0, no tagged release, no external audit.** Every capability the extension defines is implemented, and the `.phpt` suite (150 tests and counting — `make test` prints the live figure) covers compilation, the PHP↔Lua boundary, the CPU/wall-clock/memory/output budgets, the capability-gated standard library, coroutines, the virtual filesystem, `require()`, the profiler, Lua language conformance, and the adversarial cases where a script tries to catch its own limit breach.

## Why this exists

### Credit first

[`LuaSandbox`](https://www.mediawiki.org/wiki/LuaSandbox) has been running untrusted,
user-submitted Lua in front of one of the busiest sites on the internet since 2012. Every
Wikipedia infobox, citation and navbox is a Scribunto module executing inside it. That is
a decade of adversarial exposure at a scale no new project can claim, and the ideas it
proved are the ones this extension is built on: that the sandbox belongs *inside* the
interpreter rather than wrapped around it, that limits have to be enforced where the
instructions actually execute, and that a host has to assemble the standard library it
wants rather than subtract from the one it got. LuaExt is downstream of that thinking in
every way that matters. Its authors solved the hard part first, and they solved it in
public.

### So why not contribute this there?

Two reasons, and neither is a criticism of the work.

**These changes would not be good for that project's users.** Moving Lua 5.1 → 5.5 changes
the language under every Scribunto module on every wiki running it. Adding a virtual
filesystem hands a capability to an environment that has deliberately spent a decade
having none. What is a feature here would be a compatibility event and a wider attack
surface there. A project is allowed to have a narrower mandate than the thing you want to
build, and LuaSandbox's mandate is MediaWiki — correctly so.

**And the contribution path is built for a different kind of change.** LuaSandbox is
developed on [Wikimedia Gerrit](https://gerrit.wikimedia.org/r/admin/repos/mediawiki/php/luasandbox),
not GitHub; the GitHub presence is a read-only mirror. Contributing means a Wikimedia
developer account, the Gerrit patchset workflow, and review by maintainers whose priority
is — rightly — what serves the wikis. That is a reasonable amount of process for a bug
fix. For a rewrite that swaps the vendored interpreter, replaces the limit architecture
and changes the entire public API, it is the wrong shape of contribution, aimed at the
wrong project, sent through a pipeline meant for something else. The realistic outcome of
proposing it upstream is a long conversation ending in "this is a different extension" —
so it starts as one.

Hence: a separate extension, MIT-licensed, developed on GitHub, owing no wiki a
compatibility promise.

### What that freedom bought

1. **Limits that are enforced on every platform, or say they aren't.** `LuaSandbox`'s `setCPULimit()` is built on Linux-only POSIX timers; on macOS and Windows it compiles to a stub, so the call succeeds and the limit is never enforced. That is a defensible trade for an extension whose production target is Linux, and a trap for anyone else. LuaExt enforces CPU and wall-clock budgets on all three platforms, and `Sandbox::features()` reports the real per-platform enforcement level and clock resolution, so a host is never silently unprotected — see [platform support](docs/platform-support.md).
2. **A current Lua, vendored and patched in-tree.** `LuaSandbox` targets Lua 5.1. LuaExt vendors and patches **Lua 5.5.1** directly — never the system `liblua` — so the sandboxing checks live in the interpreter's hot loops instead of being bolted on from outside, which is why they cost [0–5% rather than +55%](docs/performance.md).
3. **Coroutines and a filesystem, because the interrupt can follow them.** `LuaSandbox` removed coroutines entirely because its timeout hook could not span them. LuaExt exposes them by default, capped and strictly call-scoped — the interrupt follows whichever coroutine is running, so work cannot be hidden in one. It also adds a host-implemented virtual filesystem so scripts can do `io`-style work against storage the host controls, with every path canonicalised and every quota enforced before a backend is called.

This is a from-scratch rewrite, not a fork, and there is no compatibility shim — see [migrating from LuaSandbox](docs/migrating-from-luasandbox.md).

## Requirements

- PHP **8.5** or later (NTS and ZTS). The build refuses anything older. ZTS builds compile and pass the suite, but behaviour under *real* concurrency — several worker threads running sandboxes at once, as FrankenPHP and friends do — has no test coverage yet; see [SECURITY.md](SECURITY.md)'s threading section before caching sandboxes across worker threads.
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
- [docs/playground.md](docs/playground.md) — a browser playground in one file under `examples/`: every capability, limit and quota behind a Run button, with an in-memory VFS and live stats.

**Choosing how to run it**

- [docs/platform-support.md](docs/platform-support.md) — per-platform CPU-clock precision and what `features()` reports.
- [docs/performance.md](docs/performance.md) — what the sandbox costs, measured: the interpreter against stock Lua, and a matrix across the compile cache, output modes, filesystem backend shapes and the profiler.
- [docs/migrating-from-luasandbox.md](docs/migrating-from-luasandbox.md) — the mechanical rename, and the behaviour changes that are not mechanical.

**Working on it**

- [CONTRIBUTING.md](CONTRIBUTING.md) — how to build (including the debug PHP that is the only thing able to see a leak), what every gate checks, where a new test belongs, and the C conventions that keep untrusted code contained.

**Trusting it**

- [SECURITY.md](SECURITY.md) — the threat model: what is and is not defended against, the trust model, and how to report a vulnerability.
- [CHANGELOG.md](CHANGELOG.md) — release notes (currently unreleased-only; no tags exist yet).

## License

MIT. See [LICENSE](LICENSE). The vendored Lua interpreter under `third_party/lua-5.5.1/` keeps its own upstream MIT license file — Lua is Copyright © Lua.org, PUC-Rio, distributed under the same MIT terms as this project.
