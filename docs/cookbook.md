# Cookbook

Practical recipes for building a host around `Sandbox`. Each recipe assumes you've read the [capabilities/limits overview in the README](../README.md#capabilities-and-limits) and, for anything touching the filesystem or module loading, [docs/lua-api.md](lua-api.md).

> **Status: pre-1.0, no tagged release.** These recipes are written against the pinned public API in `stubs/luaext.stub.php` and `stubs/luaext_exceptions.stub.php` — class, method, and parameter names, including on `SandboxStats` and `FileStat`, are accurate and will not drift.
>
> Every recipe here runs against a working binary: sandbox construction, `eval`/`call`/`compile`, `registerLibrary`/`registerObject`, the limits, output capture, the exception hierarchy, a `FileSystem` backend, `require()` with vendored Lua libraries, coroutine patterns, and the profiler.

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

A callback returns exactly one Lua value. Returning an array gives the script a table, not several results — `table.unpack` is how a script spreads one into many.

Note the indexing when it does. Array keys are carried across unchanged rather than renumbered, so a PHP list arrives 0-indexed and its first element sits outside what Lua counts as the sequence. A script iterating the rows above with `ipairs` would silently start at the second one; `pairs` walks all of them, and a callback that wants to hand back a Lua-idiomatic sequence should return a 1-based array (`array_combine(range(1, count($rows)), $rows)`) rather than rely on the script to know. Preserving keys is deliberate — renumbering could not survive mixed or sparse arrays, and would make the round trip lossy — but it is the sharpest edge in the conversion layer.

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

    /** @var array<string, int> */
    private array $fileModifiedAtByPath = [];

    public function exists(string $path): bool
    {
        return array_key_exists($path, $this->fileContentsByPath);
    }

    public function stat(string $path): ?FileStat
    {
        if (! $this->exists($path)) {
            return null;
        }

        return new FileStat(
            size: strlen($this->fileContentsByPath[$path]),
            mtime: $this->fileModifiedAtByPath[$path],
        );
    }

    public function read(string $path): string
    {
        return $this->fileContentsByPath[$path]
            ?? throw new VfsError("No such file: {$path}");
    }

    public function write(string $path, string $contents): void
    {
        $this->fileContentsByPath[$path] = $contents;
        $this->fileModifiedAtByPath[$path] = time();
    }

    public function delete(string $path): void
    {
        unset($this->fileContentsByPath[$path], $this->fileModifiedAtByPath[$path]);
    }

    public function rename(string $fromPath, string $toPath): void
    {
        $this->fileContentsByPath[$toPath] = $this->read($fromPath);
        $this->fileModifiedAtByPath[$toPath] = $this->fileModifiedAtByPath[$fromPath];
        unset($this->fileContentsByPath[$fromPath], $this->fileModifiedAtByPath[$fromPath]);
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
    private readonly string $contentHashKey;

    private readonly string $modifiedAtHashKey;

    public function __construct(
        private readonly Redis $redisConnection,
        private readonly string $tenantKeyPrefix,
    ) {
        $this->contentHashKey = "{$this->tenantKeyPrefix}:content";
        $this->modifiedAtHashKey = "{$this->tenantKeyPrefix}:mtime";
    }

    public function exists(string $path): bool
    {
        return (bool) $this->redisConnection->hExists($this->contentHashKey, $path);
    }

    public function stat(string $path): ?FileStat
    {
        $contents = $this->redisConnection->hGet($this->contentHashKey, $path);

        if ($contents === false) {
            return null;
        }

        $modifiedAtUnixTimestamp = (int) $this->redisConnection->hGet($this->modifiedAtHashKey, $path);

        return new FileStat(size: strlen($contents), mtime: $modifiedAtUnixTimestamp);
    }

    public function read(string $path): string
    {
        $contents = $this->redisConnection->hGet($this->contentHashKey, $path);

        return $contents !== false ? $contents : throw new VfsError("No such file: {$path}");
    }

    public function write(string $path, string $contents): void
    {
        $this->redisConnection->hSet($this->contentHashKey, $path, $contents);
        $this->redisConnection->hSet($this->modifiedAtHashKey, $path, (string) time());
    }

    public function delete(string $path): void
    {
        $this->redisConnection->hDel($this->contentHashKey, $path);
        $this->redisConnection->hDel($this->modifiedAtHashKey, $path);
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
        $allPaths = array_keys($this->redisConnection->hGetAll($this->contentHashKey) ?: []);

        return array_values(array_filter(
            $allPaths,
            static fn (string $path): bool => str_starts_with($path, $directoryPrefix),
        ));
    }
}
```

`$tenantKeyPrefix` — a pair of Redis hashes per tenant, one for content and one for modification times — is what keeps one host's sandboxes from ever seeing another's files; construct a fresh `RedisFileSystem` per tenant rather than sharing one across sandboxes with different owners.

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

    public function resolve(string $moduleName, string $requestedByChunkName): ?ModuleSource
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

// eval() is #[\NoDiscard]; cast to (void) when its multi-return array isn't needed.
(void) $sandbox->eval('print("hello from lua")');

$capturedOutput = $sandbox->takeOutput();
```

`getOutput()` reads without clearing; `takeOutput()` reads and clears in one step — useful when a sandbox handles several `call()`s and you want per-call output rather than an ever-growing buffer. `isOutputTruncated()` tells you whether `Limits::outputBytes` was hit under `OverflowBehavior::Truncate`.

> **`Limits::$outputBytes` bounds what is buffered at once, not what a script may produce in total.** `takeOutput()` hands over the bytes *and* the budget they occupied, so a host draining in a loop gives the script room to print again — which is the point of draining. The truncation flag deliberately does **not** reset: a host that took a truncated buffer still needs to know it was incomplete.
>
> The consequence worth planning for: a host that calls `takeOutput()` from inside an output callback, or in a loop around `call()`, places no ceiling on a script's *cumulative* output. Memory stays bounded, because only one buffer exists at a time; wall-clock and CPU limits still apply. If you need a total cap, count the bytes you drain and stop calling in yourself — the extension deliberately does not guess a policy here.

Streaming, for a script whose output should reach a client incrementally rather than all at once at the end:

```php
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\OutputMode;
use DevelopGravity\LuaExt\Sandbox;
use DevelopGravity\LuaExt\SandboxConfig;

$sandbox = new Sandbox(new SandboxConfig(
    outputMode: OutputMode::Callback,
    outputCallback: static function (string $outputChunk, bool $isStandardError): void {
        // Flush immediately — e.g. onto an open SSE or WebSocket connection.
        // $isStandardError distinguishes io.stderr writes from print()/io.stdout ones.
        fwrite($isStandardError ? STDERR : STDOUT, $outputChunk);
        flush();
    },
    outputChunkBytes: 4096,
));

// call() is #[\NoDiscard]; cast to (void) when its multi-return array isn't needed.
(void) $sandbox->call('run');
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

        // SandboxStats is JsonSerializable, so it can be logged or billed without
        // hand-mapping fields: memoryBytes, peakMemoryBytes, memoryLimitBytes,
        // cpuSeconds, wallClockSeconds, outputBytes, outputTruncated,
        // liveCoroutines, peakCoroutineDepth, modulesLoaded, vfsOperations,
        // vfsBytes, gcCollections, luaCallsIn, and phpCallsOut.
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

## Validating a script before you store it

A host that lets people author Lua wants to reject a broken script when it is **saved**,
not when it next runs — and to show the author which line to fix. `validate()` answers
that as data rather than by throwing, so it fits an ordinary form-validation path:

```php
use DevelopGravity\LuaExt\Limits;
use DevelopGravity\LuaExt\Sandbox;
use DevelopGravity\LuaExt\SandboxConfig;

$sandbox = new Sandbox(new SandboxConfig(limits: new Limits(maxSourceBytes: 64 * 1024)));

$result = $sandbox->validate($submittedSource, '@user-rule-' . $ruleId . '.lua');

if (!$result->valid) {
    return back()->withErrors([
        'source' => sprintf('Line %d: %s', $result->line ?? 0, $result->message),
    ]);
}
```

**Give the chunk name a `@` or `=` prefix.** Lua reads the first character of a chunk name
as a mode flag, and that choice decides whether you get a line number back:

| Chunk name | Lua displays it as | `line` / `chunkName` |
|---|---|---|
| `'@rule-42.lua'` | `rule-42.lua` | reported |
| `'=(load)'` | `(load)` | reported |
| `'rule-42.lua'` | `rule-42.lua` | reported — **normalised to `@rule-42.lua`** |

An unprefixed name would be *source text* to Lua, quoted as `[string "..."]`, leaving no
name to match against its message and therefore no line to report. **`validate()` normalises
it**, because reporting a position is the method's whole purpose.

That normalisation is `validate()`-only, and deliberately so: `compile()` and `eval()` are
thin wrappers over Lua's loader and keep its convention, so `compile($src, 'rule.lua')` still
reports `[string "rule.lua"]` and a null chunk name. Passing a prefix explicitly keeps every
method in agreement.

Three things make it fit that job rather than `compile()` in a `try`/`catch`:

- **Nothing runs.** The chunk is parsed and discarded. A script whose top level would
  delete something cannot do so by being validated.
- **The sandbox's limits apply**, because it is an instance method. `maxSourceBytes` above
  refuses an oversized submission before the parser sees it — and reports `valid: false`
  with a `line` of `null`, since a size refusal has no position in the file.
- **Only a parse failure is data.** A closed sandbox or a cross-thread call still throws:
  those say something went wrong on the *host*, and reporting them as "your Lua is
  invalid" would send the author looking at code that is fine.

`ValidationResult` is `JsonSerializable`, so it can go straight back to a form or an API:

```php
echo json_encode($sandbox->validate('return ((', '@draft.lua'));
// {"valid":false,"message":"draft.lua:1: unexpected symbol near <eof>","line":1,"chunkName":"draft.lua"}
```

Validating does not guarantee the script will *succeed* — it only guarantees it parses.
Runtime failures, and every resource limit, still apply when it actually runs.

## Not paying to compile the same script twice

`eval()` parses its source on every call. On a 3.5 KB script that is ~79 µs per call, so a
loop or a long-lived worker pays for the same parse over and over. There are two ways out,
and which applies depends entirely on whether your sandbox outlives the work.

### If the sandbox is long-lived: turn the cache on

```php
$sandbox = new Sandbox(new SandboxConfig(
    limits: new Limits(maxCachedChunks: 64),   // the default
    cacheCompiledChunks: true,
));
```

Measured **5.6×** on that 3.5 KB script (84.3 µs → 15.0 µs). Cached chunks are billed
against `memoryBytes` and counted by `stats()->cachedChunks`, and past `maxCachedChunks`
evaluation keeps working and simply stops caching — a full cache never turns a working
call into a failing one.

**It does nothing for a sandbox built per request.** That shape starts with an empty
cache every time, so it pays the parse anyway and the retention is pure cost. The setting
is off by default for exactly this reason: it is a win for one usage shape and a waste in
the other, and only you know which you have.

### If the sandbox is per-request: cache sealed bytecode

`compile()` + `dump()` produces a binary chunk that `compileBinary()` loads far faster
than parsing, and the gap widens as scripts grow:

| Script | Parse | Sealed (checksum) | Sealed (HMAC) |
|---|---:|---:|---:|
| 3.8 KB | 87 µs | **11 µs** (7.6×) | 33 µs (2.6×) |
| 200 KB | 4348 µs | **488 µs** (8.9×) | 1602 µs (2.7×) |

Bytecode is dangerous to load, so everything `dump()` produces is sealed and
`compileBinary()` verifies it first. The default seal is an unkeyed **xxh128 checksum**:
no key to manage, no INI to open, and 25 µs on a 297 KB blob. Only blobs from *elsewhere*
— `string.dump()` output, a build step — need `luaext.allow_raw_bytecode=1`.

```php
$cache = new InProcessBytecodeCache($capabilities);   // no key needed
```

Switch to `SealMode::Authenticated` when the store might be reachable by another process:
it seals with HMAC-SHA256 over a key you supply, so a blob sealed under one key will not
load under another and accidental sharing fails closed instead of working. It costs about
48× the checksum (1219 µs against 25 µs on 297 KB), which is still ahead of a 4.4 ms
parse.

```php
use DevelopGravity\LuaExt\Capabilities;
use DevelopGravity\LuaExt\Sandbox;
use DevelopGravity\LuaExt\SandboxConfig;

final class SealedBytecodeCache
{
    /** @var array<string, string> */
    private array $compiled = [];

    public function __construct(private readonly Capabilities $capabilities)
    {
    }

    public function sandbox(): Sandbox
    {
        // Default SealMode::Checksum: no key, and dump() still seals.
        return new Sandbox(new SandboxConfig(capabilities: $this->capabilities));
    }

    public function bytecodeFor(string $source, string $chunkName): string
    {
        $cacheKey = hash('xxh128', $chunkName . "\0" . $source);

        if (!isset($this->compiled[$cacheKey])) {
            $builder = $this->sandbox();

            try {
                $this->compiled[$cacheKey] = $builder->compile($source, $chunkName)->dump(true);
            } finally {
                $builder->close();
            }
        }

        return $this->compiled[$cacheKey];
    }
}
```

Every request then builds a sandbox from `sandbox()`, calls
`compileBinary($cache->bytecodeFor(...))`, and pays a verification instead of a parse.

**What the seal is protecting you from.** Lua's loader validates a binary chunk's header
and stops — not its opcodes, register indices or jump targets. Flipping one byte at each
position of a 118-byte chunk and loading each:

| Outcome | Unsealed, 150 B | Unsealed, 297 KB | Sealed, either |
|---|---:|---:|---:|
| Refused cleanly | 57% | 17% | **100%** |
| Ran, right answer anyway | 23% | 82% | 0% |
| **Ran, WRONG answer** | 8% | — | 0% |
| Killed the process | 13% | 2% | 0% |

**It gets worse as scripts get bigger.** The loader validates the header and little else, so
the checked fraction shrinks as the blob grows: on a 297 KB chunk only 17% of single-byte
corruptions were refused and 82% loaded and ran. Silent wrong answers are the dominant
outcome at scale, and they are worse than crashes because nothing tells you.

A crafted chunk is worse again than a corrupted one: it is arbitrary native code in your
PHP workers.

**Where the key must and must not go.** Keep it in process memory — `random_bytes(32)` at
startup, as above. A blob sealed under one key will not load under another, so a shared
store stops being a way in: whatever an attacker writes there will not verify. That
property is the reason to seal rather than to be careful.

It does **not** survive host compromise. An attacker who can read your process memory has
the key, and a host that caches the key beside the bytecode has authenticated nothing.

**If you genuinely need raw, unsealed bytecode** — loading blobs from a build step, say —
an operator must set `luaext.allow_raw_bytecode=1` in `php.ini`. It is `PHP_INI_SYSTEM`,
so no application code can turn it on, and `phpinfo()` reports whether it is open. Prefer
sealing: it is the same speed and none of the exposure.

## Running Lua from a queued job

A `Sandbox` wraps a live `lua_State` — a C heap outside PHP's allocator, pinned to the
thread that made it — so it cannot be serialized, exactly as a PDO connection or a curl
handle cannot. Queue the **configuration**, and build the sandbox on the worker.

```php
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Capabilities;
use DevelopGravity\LuaExt\Limits;
use DevelopGravity\LuaExt\Sandbox;
use DevelopGravity\LuaExt\SandboxConfig;
use DevelopGravity\LuaExt\Exception\LuaException;
use Illuminate\Contracts\Queue\ShouldQueue;
use Illuminate\Foundation\Queue\Queueable;

final class RunUserScript implements ShouldQueue
{
    use Queueable;

    public function __construct(
        private readonly string $source,
        private readonly SandboxConfig $config,
    ) {}

    public function handle(): void
    {
        // Built here, never carried in the payload.
        $sandbox = new Sandbox($this->config);

        try {
            [$result] = $sandbox->eval($this->source, '@user-script.lua');

            // ... persist $result ...
        } catch (LuaException $error) {
            // Survives the boundary: message, code, and the Lua traceback are all
            // intact if this exception is itself serialized by the failed-job
            // machinery. getSandbox() is null on the far side, deliberately.
            report($error);

            throw $error;
        } finally {
            $sandbox->close();
        }
    }
}

RunUserScript::dispatch(
    'return 6 * 7',
    new SandboxConfig(limits: new Limits(cpuSeconds: 2.0, memoryBytes: 16 * 1024 * 1024)),
);
```

`Limits`, `Capabilities`, `VfsQuota` and `SandboxConfig` are plain readonly value objects
and serialize normally — pinned by `tests/01-basic/config-objects-survive-a-queue.phpt` so
it stays that way.

**The one thing that cannot travel is an output callback.** `SandboxConfig::$outputCallback`
is typed `?\Closure`, and PHP refuses to serialize closures. `laravel/serializable-closure`
does not help, because a `SerializableClosure` is not a `Closure` and will not satisfy the
type. Queue the config without a callback and attach the behaviour worker-side — use
`OutputMode::Buffer` and read `takeOutput()` after the call, which is the shape that
survives a queue anyway.

**In-process deferred work is unaffected.** `defer()`, `terminating()` and
`register_shutdown_function` all run in the same process and serialize nothing, so a live
sandbox is still there. Only a real queue worker crosses a process boundary.

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
