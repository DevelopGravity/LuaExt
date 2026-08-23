# Cookbook

Practical recipes for building a host around `Sandbox`. Each recipe assumes you've read the [capabilities/limits overview in the README](../README.md#capabilities-and-limits) and, for anything touching the filesystem or module loading, [docs/lua-api.md](lua-api.md).

> **Status: pre-1.0, no working build yet.** These recipes are written against the approved API design, not a tested build — treat method and namespace names as the intended target, cross-check against `developgravity/lua-extension-stubs` once it's published, and expect exact field/parameter names on `SandboxStats`, `FileStat`, and similar plain data objects to be confirmed rather than assumed from this document alone.

## Exposing host services via `registerObject()`

A sandboxed script can never `dlopen` a binary LuaRocks module — that's an architectural property of the vendored, hidden-visibility Lua build, not a policy switch (see [SECURITY.md](../SECURITY.md#what-this-does-not-defend-against)). So there is no way to get, say, a real `sqlite` LuaRocks module running inside a sandbox. The supported pattern instead is: implement the capability as ordinary PHP, expose the specific methods you want reachable, and let the extension handle the boundary.

`registerObject(string $name, object $instance, ?array $methods = null)` publishes an existing object to Lua as a table of bound method callables. Selection is explicit only — either pass an allowlist of method names, or mark methods with `#[LuaMethod]` (optionally renaming what Lua sees: `#[LuaMethod('query')]`). An object with neither throws `ConfigurationError` at registration time; there's no implicit "expose everything public" mode.

### Example: a read-only SQLite query service

This is how you get "SQL in Lua" without a binary rock: the sandbox never touches SQLite directly, it calls back into PHP, and PHP does the actual querying against a connection the host fully controls.

```php
<?php

declare(strict_types=1);

namespace App\Lua;

use DevelopGravity\LuaExt\Exception\RuntimeError;
use DevelopGravity\LuaExt\LuaMethod;
use PDO;
use PDOException;

final class ReadOnlyQueryService
{
    public function __construct(
        private readonly PDO $readOnlyDatabaseConnection,
    ) {
    }

    /**
     * Run a single read-only SELECT statement and return every matching row.
     *
     * Defense in depth, in order: the connection itself should be opened against
     * a read-only DSN (e.g. SQLite's `sqlite:/path/to.db?mode=ro` URI form, or a
     * MySQL/Postgres user grant with SELECT only); the statement-prefix check
     * below is a fast, cheap rejection, not the actual security boundary; and
     * PDO::prepare() never executes more than one statement per call, so
     * stacked-query injection isn't reachable even if the prefix check were
     * bypassed.
     *
     * @param array<int, int|float|string|null> $boundParameters Positional bind values.
     * @return array<int, array<string, int|float|string|null>>
     * @throws RuntimeError When the statement isn't a SELECT, or the query fails.
     */
    #[LuaMethod('query')]
    public function runSelectQuery(string $sqlQuery, array $boundParameters = []): array
    {
        if (! preg_match('/^\s*SELECT\b/i', $sqlQuery)) {
            throw new RuntimeError('Only SELECT statements are permitted.');
        }

        try {
            $preparedStatement = $this->readOnlyDatabaseConnection->prepare($sqlQuery);
            $preparedStatement->execute($boundParameters);

            return $preparedStatement->fetchAll(PDO::FETCH_ASSOC);
        } catch (PDOException $databaseException) {
            throw new RuntimeError(
                message: "Query failed: {$databaseException->getMessage()}",
                previous: $databaseException,
            );
        }
    }
}
```

```php
<?php

declare(strict_types=1);

use App\Lua\ReadOnlyQueryService;
use DevelopGravity\LuaExt\Sandbox;

$readOnlyConnection = new PDO('sqlite:/var/data/widgets.db?mode=ro');

$sandbox = new Sandbox();
$sandbox->registerObject('database', new ReadOnlyQueryService($readOnlyConnection));

[$widgetRows] = $sandbox->eval(<<<'LUA'
    return database.query("SELECT id, name FROM widgets WHERE active = ?", {1})
LUA);
```

`RuntimeError` is the one exception type a callback should throw for a condition the script is meant to handle — a malformed query, a permission problem, anything a Lua-side `pcall` should be able to catch. Throwing anything else (or letting an unexpected exception escape) is fatal to the call, and the original PHP exception is preserved end-to-end and rethrown to the host with a Lua traceback attached, per the extension's callback contract.

Object identity never crosses the boundary either direction — Lua gets bound closures, never a reference to `$instance` it could otherwise introspect, and passing a PHP object as an argument into Lua is always a `ConversionError`. This is deliberate: `registerObject`/`registerLibrary` is the *only* bridge, and it's a one-way, method-at-a-time one.

## Implementing a `FileSystem` backend

Granting the `vfs` capability gives a script a conventional-looking `io` library backed by a `FileSystem` implementation you provide. The interface is intentionally blob-oriented — no handles, no offsets, no quota checks — because all of that lives in the C VFS layer, which enforces `VfsQuota` (open handles, per-file/total byte caps, file count, operation count, path length/depth) *before* your backend ever sees a call. That's why backend implementations stay small: a backend author never re-implements quota logic, and a backend that has no built-in limits of its own is still fully protected by the extension.

```php
<?php

declare(strict_types=1);

namespace App\Lua;

use DevelopGravity\LuaExt\Exception\VfsError;
use DevelopGravity\LuaExt\FileStat;
use DevelopGravity\LuaExt\FileSystem;

final class InMemoryFileSystem implements FileSystem
{
    /** @var array<string, string> */
    private array $fileContentsByPath = [];

    public function exists(string $path): bool
    {
        return array_key_exists($path, $this->fileContentsByPath);
    }

    public function stat(string $path): ?FileStat
    {
        if (! $this->exists($path)) {
            return null;
        }

        return new FileStat(sizeInBytes: strlen($this->fileContentsByPath[$path]));
    }

    public function read(string $path): string
    {
        return $this->fileContentsByPath[$path]
            ?? throw new VfsError("No such file: {$path}");
    }

    public function write(string $path, string $contents): void
    {
        $this->fileContentsByPath[$path] = $contents;
    }

    public function delete(string $path): void
    {
        unset($this->fileContentsByPath[$path]);
    }

    public function rename(string $fromPath, string $toPath): void
    {
        $this->fileContentsByPath[$toPath] = $this->read($fromPath);
        unset($this->fileContentsByPath[$fromPath]);
    }

    /** @return array<int, string> */
    public function list(string $directoryPath): array
    {
        $directoryPrefix = rtrim($directoryPath, '/') . '/';

        return array_values(array_filter(
            array_keys($this->fileContentsByPath),
            static fn (string $path): bool => str_starts_with($path, $directoryPrefix),
        ));
    }
}
```

Throwing `VfsError` (a `RuntimeError` subclass) for a missing file keeps the failure script-catchable — the VFS layer maps it to the conventional Lua `nil, message, code` shape rather than aborting the call. Throwing anything else from a backend method is treated as fatal, on the theory that a storage-layer exception (a dropped connection, say) is a host problem, not something a script should be expected to `pcall` around.

A Redis-backed variant follows the same shape; the interesting difference is that isolating tenants is entirely the backend's responsibility — the VFS layer only canonicalizes paths within whatever namespace a backend chooses to expose, it has no concept of "which customer" on its own:

```php
<?php

declare(strict_types=1);

namespace App\Lua;

use DevelopGravity\LuaExt\Exception\VfsError;
use DevelopGravity\LuaExt\FileStat;
use DevelopGravity\LuaExt\FileSystem;
use Redis;

final class RedisFileSystem implements FileSystem
{
    public function __construct(
        private readonly Redis $redisConnection,
        private readonly string $tenantKeyPrefix,
    ) {
    }

    public function exists(string $path): bool
    {
        return (bool) $this->redisConnection->hExists($this->tenantKeyPrefix, $path);
    }

    public function stat(string $path): ?FileStat
    {
        $contents = $this->redisConnection->hGet($this->tenantKeyPrefix, $path);

        return $contents === false ? null : new FileStat(sizeInBytes: strlen($contents));
    }

    public function read(string $path): string
    {
        $contents = $this->redisConnection->hGet($this->tenantKeyPrefix, $path);

        return $contents !== false ? $contents : throw new VfsError("No such file: {$path}");
    }

    public function write(string $path, string $contents): void
    {
        $this->redisConnection->hSet($this->tenantKeyPrefix, $path, $contents);
    }

    public function delete(string $path): void
    {
        $this->redisConnection->hDel($this->tenantKeyPrefix, $path);
    }

    public function rename(string $fromPath, string $toPath): void
    {
        $this->write($toPath, $this->read($fromPath));
        $this->delete($fromPath);
    }

    /** @return array<int, string> */
    public function list(string $directoryPath): array
    {
        $directoryPrefix = rtrim($directoryPath, '/') . '/';
        $allPaths = array_keys($this->redisConnection->hGetAll($this->tenantKeyPrefix) ?: []);

        return array_values(array_filter(
            $allPaths,
            static fn (string $path): bool => str_starts_with($path, $directoryPrefix),
        ));
    }
}
```

`$tenantKeyPrefix` — one Redis hash per tenant — is what keeps one host's sandboxes from ever seeing another's files; construct a fresh `RedisFileSystem` per tenant rather than sharing one across sandboxes with different owners.

For a backend that can stream ranges instead of buffering whole files, implement the optional `RangedFileSystem extends FileSystem` (`readRange`, `writeRange`, `truncate`) instead — worth doing once files can meaningfully exceed `VfsQuota::maxFileBytes` and buffering the whole blob per operation stops being cheap.

## Vendoring pure-Lua libraries

Pure-Lua libraries — including pure-Lua LuaRocks packages like `dkjson`, `penlight`, or `inspect` — run inside the sandbox just fine, under the same limits as the rest of the script, because they're nothing but ordinary Lua source. The recommended way to serve them is a small `ModuleResolver` that reads straight from a vendored directory you commit into your own repository — this needs only the `require` capability, not `vfs`/`vfsWrite`, since the library code isn't part of the sandbox's own virtual filesystem at all:

```php
<?php

declare(strict_types=1);

namespace App\Lua;

use DevelopGravity\LuaExt\ModuleResolver;
use DevelopGravity\LuaExt\ModuleSource;

final readonly class VendoredLuaLibraryResolver implements ModuleResolver
{
    public function __construct(
        private string $vendorDirectoryPath,
    ) {
    }

    public function resolve(string $moduleName, ?string $requestedByChunkName): ?ModuleSource
    {
        $relativeSourcePath = str_replace('.', '/', $moduleName) . '.lua';
        $absoluteSourcePath = "{$this->vendorDirectoryPath}/{$relativeSourcePath}";

        if (! is_file($absoluteSourcePath)) {
            return null;
        }

        return new ModuleSource(
            code: file_get_contents($absoluteSourcePath),
            chunkName: "=vendor/{$moduleName}",
            isBytecode: false,
        );
    }
}
```

```php
<?php

declare(strict_types=1);

use App\Lua\VendoredLuaLibraryResolver;
use DevelopGravity\LuaExt\Capabilities;
use DevelopGravity\LuaExt\Sandbox;
use DevelopGravity\LuaExt\SandboxConfig;

$sandbox = new Sandbox(new SandboxConfig(
    capabilities: (new Capabilities())->with(require: true),
    moduleResolver: new VendoredLuaLibraryResolver(__DIR__ . '/../vendor-lua'),
));

[$encodedJson] = $sandbox->eval('return require("dkjson").encode({hello = "world"})');
```

To vendor a rock: extract its `.lua` sources (LuaRocks packages are ordinary source trees; no LuaRocks or C toolchain is needed to just copy the files out) into `vendor-lua/`, matching the module name a script would `require()`. Because resolution falls back through `preload` → VFS `modulePaths` → your `ModuleResolver` in that order (see [docs/lua-api.md](lua-api.md#require-semantics)), you can also serve vendored libraries from the VFS itself under `SandboxConfig::modulePaths` (default `['/?.lua', '/?/init.lua']`) if you'd rather keep library code alongside user-authored VFS content — that route does need `vfs` granted, since it's the same virtual filesystem user scripts see.

Binary (C) LuaRocks are not an option under either approach — see [SECURITY.md](../SECURITY.md#what-this-does-not-defend-against) for why that's architectural rather than a missing resolver feature.

## Capturing or streaming output

`SandboxConfig::outputMode` controls what happens to `print`/`io.write`/`io.stderr` output. `OutputMode::Discard` is cheapest when a script's output doesn't matter to you at all.

Buffered capture, retrieved after the call:

```php
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\OutputMode;
use DevelopGravity\LuaExt\Sandbox;
use DevelopGravity\LuaExt\SandboxConfig;

$sandbox = new Sandbox(new SandboxConfig(outputMode: OutputMode::Buffer));

$sandbox->eval('print("hello from lua")');

$capturedOutput = $sandbox->takeOutput();
```

`getOutput()` reads without clearing; `takeOutput()` reads and clears in one step — useful when a sandbox handles several `call()`s and you want per-call output rather than an ever-growing buffer. `isOutputTruncated()` tells you whether `Limits::outputBytes` was hit under `OverflowBehavior::Truncate`.

Streaming, for a script whose output should reach a client incrementally rather than all at once at the end:

```php
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\OutputMode;
use DevelopGravity\LuaExt\Sandbox;
use DevelopGravity\LuaExt\SandboxConfig;

$sandbox = new Sandbox(new SandboxConfig(
    outputMode: OutputMode::Callback,
    outputCallback: static function (string $outputChunk): void {
        // Flush immediately — e.g. onto an open SSE or WebSocket connection.
        echo $outputChunk;
        flush();
    },
    outputChunkBytes: 4096,
));

$sandbox->call('run');
```

Callback chunks flush at whichever comes first: the `outputChunkBytes` threshold, a newline, the outermost `call()`/`eval()` returning, or `close()`. If `Limits::outputOverflow` is `OverflowBehavior::Fail` instead of `Truncate`, exceeding `outputBytes` raises an *uncatchable* `OutputLimitError` — a script can't wrap its own `print` calls in `pcall` to buy itself unlimited output.

## Recording per-script resource usage

`Sandbox::stats(): SandboxStats` returns a readonly, `JsonSerializable` snapshot intended specifically for logging and billing pipelines — it's meant to be handed straight to a metrics client rather than picked apart field by field. It's callable mid-run from inside a PHP callback, after a call completes, and any time up until `close()`.

```php
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Sandbox;

final class MeteredLuaRunner
{
    public function __construct(
        private readonly MetricsClient $metricsClient,
    ) {
    }

    public function runScript(Sandbox $sandbox, string $scriptEntryPoint): array
    {
        $returnValues = $sandbox->call($scriptEntryPoint);

        $usageSnapshot = $sandbox->stats();

        // SandboxStats is JsonSerializable, so it can be logged or billed
        // without hand-mapping fields — the plan guarantees at least: memory
        // usage/peak/limit, cpuSeconds, wallClockSeconds, outputBytes (+
        // truncated flag), live/peak coroutine counts, modulesLoaded,
        // vfsOperations/vfsBytes, gcCollections, and luaCallsIn/phpCallsOut.
        $this->metricsClient->record('lua.sandbox.usage', $usageSnapshot);

        return $returnValues;
    }
}
```

Billing on CPU time consumed is a straightforward multiplication once you have the snapshot:

```php
$estimatedCostInUsd = $usageSnapshot->cpuSeconds * self::PRICE_PER_CPU_SECOND_IN_USD;
```

Because `stats()` stays readable until `close()`, a long-running or multi-call sandbox can be sampled progressively (e.g. from an output callback) for near-real-time metering rather than only a single end-of-run total.

## Coroutine patterns

Coroutines fit naturally into the strictly call-scoped model as long as they're used for in-script control flow that starts and finishes within one `call()`/`eval()` — generators, iterators, small cooperative schedulers. What they cannot do is survive past that call, or suspend across a PHP callback.

A generator/iterator, entirely conventional Lua:

```lua
local function fibonacciSequence(count)
    return coroutine.wrap(function()
        local previous, current = 0, 1
        for _ = 1, count do
            coroutine.yield(current)
            previous, current = current, previous + current
        end
    end)
end

for value in fibonacciSequence(10) do
    print(value)
end
```

An in-script cooperative scheduler — several coroutines taking turns within a single call, still fully synchronous from PHP's perspective:

```lua
local function makeWorker(workerName, stepCount)
    return coroutine.create(function()
        for step = 1, stepCount do
            print(workerName .. " step " .. step)
            coroutine.yield()
        end
    end)
end

local workers = {makeWorker("A", 3), makeWorker("B", 2)}

while #workers > 0 do
    for index = #workers, 1, -1 do
        local worker = workers[index]
        coroutine.resume(worker)
        if coroutine.status(worker) == "dead" then
            table.remove(workers, index)
        end
    end
end
```

**Nothing here continues after `call()` returns.** Every coroutine created during a call — however deep the nesting, whatever its status — is force-closed the moment the outermost `call()`/`eval()` returns, success, error, or timeout alike (see [docs/lua-api.md](lua-api.md#coroutines)). If a script stashes a coroutine in a global, that reference is a dead thread by the next call; resuming it raises Lua's ordinary "cannot resume dead coroutine" error, not a resurrection of old state.

That also means there is no way to hand a live coroutine to PHP and resume it later — a coroutine value crossing the boundary is a `ConversionError`, by design; there is no PHP-side handle to await, poll, or resume out of band. Anything that actually needs to run in the background — a job queue, a long poll, work that spans multiple requests — belongs to PHP-native async tooling (amphp, ReactPHP, Fibers) sitting *outside* the sandbox, which calls `Sandbox::call()` synchronously for each discrete unit of work it needs Lua to do. Trying to model background work as a coroutine a script squirrels away for later does not work, and isn't a bug to work around — it's the guarantee that a sandbox holds zero execution state once control returns to PHP.
