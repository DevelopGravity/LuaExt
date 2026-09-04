<?php

/**
 * Public API of the luaext extension.
 *
 * This file is the single source of truth for the API: it generates the C
 * arginfo consumed by the build and the IDE stub package published for editors
 * and static analysers, so a signature can never drift from the binary.
 *
 * Note: gen_stub.php rejects `declare` and `use` statements, so this file has
 * neither; names are written relative to the namespace instead. Typing is
 * governed by the generated arginfo, not by a strict_types declaration here.
 *
 * @generate-class-entries
 */

namespace DevelopGravity\LuaExt;

/**
 * Where a script's `print()` and `io.write()` output goes.
 */
enum OutputMode
{
    /** Accumulate in the sandbox; read it back with Sandbox::takeOutput(). */
    case Buffer;

    /** Stream to the callback configured on SandboxConfig, in chunks. */
    case Callback;

    /** Discard silently. */
    case Discard;
}

/**
 * What happens when a script exceeds its output budget.
 */
enum OverflowBehavior
{
    /** Drop the excess and set the truncated flag. */
    case Truncate;

    /**
     * Abort with an OutputLimitError. Fatal, so a script cannot pcall its way
     * past its own budget.
     */
    case Fail;
}

/**
 * Unit for the figures returned by Sandbox::getProfile().
 */
enum ProfilerUnit
{
    case Samples;
    case Seconds;
    case Percent;
}

/**
 * How well the running platform can enforce a given limit.
 *
 * Reported by Sandbox::features() so a host can react to a weak platform
 * instead of assuming a limit is in force when it is not.
 */
enum LimitSupport
{
    /** Enforced at the platform's full clock resolution. */
    case Enforced;

    /**
     * Enforced, but accounted coarsely; the sandbox arms a wall-clock deadline
     * as a backstop so a runaway script is still stopped.
     */
    case Degraded;

    /** Not enforceable on this platform. */
    case Unsupported;
}

/**
 * How Sandbox::dump() seals bytecode, and what compileBinary() will accept.
 *
 * Lua's binary loader validates a chunk's header and little else, and the
 * checked fraction shrinks as blobs grow -- on a 297 KB chunk only 17% of
 * single-byte corruptions were refused and 82% loaded and ran. Sealing is how a
 * blob is vouched for before the loader sees it.
 */
enum SealMode
{
    /**
     * xxh128, unkeyed. Tamper-EVIDENT: 128 bits catch corruption essentially
     * always, at ~12 GB/s -- 25us on a 297 KB blob against 1219us for HMAC.
     *
     * Stops nobody deliberate, because anyone can recompute it. Right when the
     * bytecode never leaves the process that made it, which is the only place
     * a bytecode cache belongs.
     */
    case Checksum;

    /**
     * HMAC-SHA256, using SandboxConfig::$bytecodeKey.
     *
     * Adds what a checksum cannot: a blob sealed under one key will not load
     * under another, so a bytecode store shared between processes fails CLOSED
     * rather than silently working. Costs roughly 48x the checksum.
     *
     * Still authenticates ORIGIN, not safety, and does not survive host
     * compromise -- an attacker who can read process memory has the key.
     */
    case Authenticated;
}

/**
 * Marks a method as callable from Lua once its object is passed to
 * Sandbox::registerObject().
 *
 * Exposure is always explicit: a method with no attribute and no entry in the
 * `$methods` allowlist is invisible to scripts, so adding a public method to a
 * host class can never silently widen what untrusted code may call.
 *
 * Carries no #[Attribute] marker here because gen_stub cannot resolve constants
 * it does not itself declare; MINIT calls zend_internal_attribute_register()
 * with ZEND_ATTRIBUTE_TARGET_METHOD instead, and the published IDE stubs
 * restore the marker for editors.
 */
final class LuaMethod
{
    /** Name seen by Lua; defaults to the PHP method name. */
    public ?string $name;

    public function __construct(?string $name = null) {}
}

/**
 * What a script is permitted to do.
 *
 * The defaults are the untrusted baseline: everything dangerous is off, so
 * `new Capabilities()` is always safe. Widen deliberately with named arguments
 * or with() rather than by reaching for a broader preset.
 *
 * @strict-properties
 */
final readonly class Capabilities
{
    /** Load precompiled bytecode. Unsafe by nature: malformed bytecode can crash the interpreter. */
    public bool $loadBytecode;

    /** Expose Lua's load() so scripts can compile source at runtime. */
    public bool $compileAtRuntime;

    /** Expose string.dump() and LuaFunction::dump(). */
    public bool $dumpBytecode;

    /** Expose require(). Needs a module resolver, VFS search paths, or preloaded modules. */
    public bool $require;

    /** Expose the io/os file API backed by the host FileSystem. */
    public bool $vfs;

    /** Allow writing through the VFS; without it the filesystem is read-only. */
    public bool $vfsWrite;

    /** Expose the coroutine library. Coroutines are always scoped to a single call. */
    public bool $coroutines;

    /** Expose os.time(), os.date() and os.difftime(). */
    public bool $osTime;

    /** Expose os.getenv(), restricted to $osEnvAllowList. */
    public bool $osEnv;

    /** @var list<string> Environment variables readable when $osEnv is set. */
    public array $osEnvAllowList;

    /** Expose debug.traceback(). */
    public bool $debugTraceback;

    /** Expose debug.getinfo(), debug.getlocal() and debug.getupvalue(). */
    public bool $debugIntrospect;

    /** Expose the mutating half of the debug library. Escapes most sandbox guarantees. */
    public bool $debugMutate;

    /**
     * Expose debug.sethook(). This lets a script replace the interrupt hook the
     * CPU limit relies on, so combining it with a CPU limit is refused at
     * construction.
     */
    public bool $debugHooks;

    /** Expose the utf8 library. */
    public bool $utf8;

    /** Allow collectgarbage() tuning verbs, not just the read-only ones. */
    public bool $gcControl;

    /** Expose warn(); output is routed to the sandbox's sink, never to stderr. */
    public bool $warn;

    public function __construct(
        bool $loadBytecode = false,
        bool $compileAtRuntime = false,
        bool $dumpBytecode = false,
        bool $require = false,
        bool $vfs = false,
        bool $vfsWrite = false,
        bool $coroutines = true,
        bool $osTime = true,
        bool $osEnv = false,
        array $osEnvAllowList = [],
        bool $debugTraceback = true,
        bool $debugIntrospect = false,
        bool $debugMutate = false,
        bool $debugHooks = false,
        bool $utf8 = true,
        bool $gcControl = false,
        bool $warn = false,
    ) {}

    /**
     * The safe baseline, identical to `new Capabilities()`.
     */
    public static function untrusted(): Capabilities {}

    /**
     * A permissive preset for code you wrote yourself.
     *
     * Enables runtime compilation, require(), the VFS, debug introspection, GC
     * control and warn(). Deliberately leaves bytecode loading, debug mutation
     * and debug hooks off: each voids a guarantee the sandbox otherwise makes,
     * so they must be requested one at a time.
     */
    public static function trusted(): Capabilities {}

    /**
     * Return a copy with the named capabilities replaced.
     *
     * @throws Exception\ConfigurationError if an unknown capability is named.
     */
    public function with(mixed ...$overrides): Capabilities {}
}

/**
 * Resource ceilings for a sandbox. Null or zero means unlimited.
 *
 * @strict-properties
 */
final readonly class Limits
{
    /** Peak bytes the Lua heap plus host-side buffers may occupy. */
    public ?int $memoryBytes;

    /** CPU seconds charged to the thread running the script. */
    public ?float $cpuSeconds;

    /** Wall-clock seconds, covering time spent inside host callbacks. */
    public ?float $wallClockSeconds;

    /** Bytes a script may print before $outputOverflow applies. */
    public int $outputBytes;

    public OverflowBehavior $outputOverflow;

    /** Coroutines that may exist at once. */
    public int $maxLiveCoroutines;

    /** Depth of nested coroutine resumes. */
    public int $maxCoroutineDepth;

    /** Depth of nested calls crossing the PHP boundary. */
    public int $maxCallDepth;

    /** Modules a script may require(). */
    public int $maxModules;

    /** Depth of nested require() calls. */
    public int $maxRequireDepth;

    /** Longest single Lua string. */
    public int $maxStringLength;

    /**
     * Longest chunk that may be compiled. The parser runs before any interrupt
     * can fire, so pathological sources are bounded by length rather than CPU.
     */
    public int $maxSourceBytes;

    /** Nesting depth when converting values between PHP and Lua. */
    public int $maxConversionDepth;

    /**
     * Chunks eval()'s compile cache may hold, when SandboxConfig enables it.
     *
     * Bounds retention, not behaviour: past this many distinct chunks, eval()
     * keeps working and simply stops caching. A full cache must never be able to
     * turn a working call into a failing one.
     */
    public int $maxCachedChunks;

    public function __construct(
        ?int $memoryBytes = 33554432,
        ?float $cpuSeconds = 1.0,
        ?float $wallClockSeconds = 5.0,
        int $outputBytes = 1048576,
        OverflowBehavior $outputOverflow = OverflowBehavior::Fail,
        int $maxLiveCoroutines = 64,
        int $maxCoroutineDepth = 16,
        int $maxCallDepth = 200,
        int $maxModules = 64,
        int $maxRequireDepth = 16,
        int $maxStringLength = 67108864,
        int $maxSourceBytes = 1048576,
        int $maxConversionDepth = 64,
        int $maxCachedChunks = 64,
    ) {}

    /**
     * Return a copy with the named limits replaced.
     *
     * @throws Exception\ConfigurationError if an unknown limit is named.
     */
    public function with(mixed ...$overrides): Limits {}
}

/**
 * Ceilings applied to the virtual filesystem.
 *
 * Enforced inside the extension rather than by the host backend, so a simple or
 * buggy backend cannot be talked past them.
 *
 * @strict-properties
 */
final readonly class VfsQuota
{
    /** Files a script may hold open at once. */
    public int $maxOpenHandles;

    /** Largest single file. */
    public int $maxFileBytes;

    /** Total bytes buffered across all handles; charged against the memory limit. */
    public int $maxTotalBytes;

    /** Files that may exist in the namespace. */
    public int $maxFiles;

    /** Calls into the host backend per sandbox call. */
    public int $maxOperations;

    public int $maxPathLength;

    public int $maxPathDepth;

    /**
     * Charge time spent inside the host backend to the wall-clock deadline.
     * Off by default, so a slow storage backend does not kill a script that is
     * behaving; CPU time is always charged either way.
     */
    public bool $billWallTime;

    public function __construct(
        int $maxOpenHandles = 16,
        int $maxFileBytes = 1048576,
        int $maxTotalBytes = 8388608,
        int $maxFiles = 128,
        int $maxOperations = 10000,
        int $maxPathLength = 255,
        int $maxPathDepth = 16,
        bool $billWallTime = false,
    ) {}

    /**
     * Return a copy with the named quotas replaced.
     *
     * @throws Exception\ConfigurationError if an unknown quota is named.
     */
    public function with(mixed ...$overrides): VfsQuota {}
}

/**
 * Everything that shapes a sandbox, fixed at construction.
 *
 * @strict-properties
 */
final readonly class SandboxConfig
{
    /** Null selects the untrusted baseline. */
    public ?Capabilities $capabilities;

    /** Null selects the default Limits. */
    public ?Limits $limits;

    /** Backing store for the io/os file API; required for the vfs capability. */
    public ?FileSystem $filesystem;

    /** Null selects the default VfsQuota. */
    public ?VfsQuota $vfsQuota;

    /** Consulted by require() after preloaded modules and VFS search paths. */
    public ?ModuleResolver $moduleResolver;

    /** @var list<string> require() search patterns, `?` replaced by the module name. */
    public array $modulePaths;

    public OutputMode $outputMode;

    /** Receives (string $chunk, bool $isStderr) in OutputMode::Callback. */
    public ?\Closure $outputCallback;

    /** Bytes buffered before the output callback is invoked. */
    public int $outputChunkBytes;

    /**
     * String-hash seed. Null draws from the system CSPRNG; a fixed value makes
     * a sandbox reproducible but forfeits hash-flooding protection, so it is
     * only accepted together with $deterministic.
     */
    public ?int $seed;

    /** Fix the seed and freeze the clock, for tests and golden-file runs. */
    public bool $deterministic;

    /**
     * Keep the chunks eval() compiles, so repeating the same source skips the
     * parser. Bounded by Limits::$maxCachedChunks.
     *
     * Only helps a sandbox that OUTLIVES several eval() calls of the same
     * source. A sandbox built per request, used once and closed, compiles into
     * an empty cache every time and gains nothing.
     *
     * Off by default because a cached chunk is an ordinary Lua object charged
     * against memoryBytes: enabling it moves a sandbox closer to its own
     * ceiling, which is not something to do to a caller who did not ask.
     */
    public bool $cacheCompiledChunks;

    /**
     * Key that seals and verifies bytecode, at least 16 bytes.
     *
     * With a key set, dump() returns a sealed blob and compileBinary() accepts
     * only sealed blobs it can verify -- an unsealed one is refused rather than
     * loaded, because a silent fallback would leave the hole open while the
     * configuration says otherwise.
     *
     * Generate one per process with random_bytes(32) and keep it out of the
     * store the bytecode lives in. Sealing authenticates ORIGIN, not safety: it
     * closes corruption and tampering by anyone without the key, and does not
     * survive an attacker who can read this process's memory.
     */
    /**
     * How dump() seals, and what compileBinary() accepts.
     *
     * Checksum needs no key and is the default. Authenticated requires
     * $bytecodeKey; passing a key without it, or asking for it without a key,
     * is refused at construction rather than silently doing the other thing.
     */
    public SealMode $sealMode;

    public ?string $bytecodeKey;

    public function __construct(
        ?Capabilities $capabilities = null,
        ?Limits $limits = null,
        ?FileSystem $filesystem = null,
        ?VfsQuota $vfsQuota = null,
        ?ModuleResolver $moduleResolver = null,
        array $modulePaths = ['/?.lua', '/?/init.lua'],
        OutputMode $outputMode = OutputMode::Buffer,
        ?\Closure $outputCallback = null,
        int $outputChunkBytes = 8192,
        ?int $seed = null,
        bool $deterministic = false,
        bool $cacheCompiledChunks = false,
        SealMode $sealMode = SealMode::Checksum,
        ?string $bytecodeKey = null,
    ) {}

    /**
     * Return a copy with the named settings replaced.
     *
     * @throws Exception\ConfigurationError if an unknown setting is named.
     */
    public function with(mixed ...$overrides): SandboxConfig {}
}

/**
 * A snapshot of what a script consumed.
 *
 * Readable while a script runs (from inside a host callback), after it returns,
 * and after it fails, so the same object serves progress reporting, billing and
 * post-mortem diagnosis. Serialises to JSON for logging pipelines.
 *
 * @strict-properties
 */
final readonly class SandboxStats implements \JsonSerializable
{
    /** Live bytes: Lua heap plus host-side buffers charged to the sandbox. */
    public int $memoryBytes;

    public int $peakMemoryBytes;

    /** Zero when unlimited. */
    public int $memoryLimitBytes;

    public float $cpuSeconds;

    public float $wallClockSeconds;

    public int $outputBytes;

    public bool $outputTruncated;

    public int $liveCoroutines;

    public int $peakCoroutineDepth;

    public int $modulesLoaded;

    /**
     * Chunks currently held by eval()'s compile cache.
     *
     * Always 0 unless SandboxConfig::$cacheCompiledChunks is set, and the only
     * way to see whether the cache is doing anything.
     */
    public int $cachedChunks;

    public int $vfsOperations;

    /** Bytes read and written through the virtual filesystem. */
    public int $vfsBytes;

    public int $gcCollections;

    /** Calls from PHP into Lua. */
    public int $luaCallsIn;

    /** Calls from Lua back into PHP. */
    public int $phpCallsOut;

    private function __construct() {}

    /** @return array<string, int|float|bool> */
    public function jsonSerialize(): array {}
}

/**
 * The outcome of Sandbox::validate(): whether a chunk parses, and if not, where.
 *
 * Deliberately not an exception. Validating user-submitted source is a routine
 * operation whose "no" is data to show an author, not a control-flow event to
 * catch -- a host saving a script wants the line to put a marker on, in the same
 * request, without a try/catch around every save.
 *
 * Publicly constructible, unlike SandboxStats: that class refuses because a
 * snapshot nothing measured would be indistinguishable from a real one, while
 * this makes no claim beyond its own fields and a host wrapping its own checks
 * has a fair reason to build one.
 *
 * @strict-properties
 */
final readonly class ValidationResult implements \JsonSerializable
{
    /** Whether the chunk compiles. Everything below is null when it does. */
    public bool $valid;

    /** The parser's message, verbatim, including its own "name:line:" prefix. */
    public ?string $message;

    /**
     * The line the parser stopped on.
     *
     * Null when the refusal has no line to report -- a chunk rejected for
     * exceeding maxSourceBytes never reached the parser at all.
     */
    public ?int $line;

    /** The chunk name as Lua displays it, without the '=' or '@' prefix. */
    public ?string $chunkName;

    public function __construct(
        bool $valid = true,
        ?string $message = null,
        ?int $line = null,
        ?string $chunkName = null,
    ) {}

    /** @return array{valid: bool, message: ?string, line: ?int, chunkName: ?string} */
    public function jsonSerialize(): array {}
}

/**
 * An isolated Lua interpreter.
 *
 * A sandbox belongs to the thread that created it; only interrupt() may be
 * called from another thread. Nothing runs in the background: when a call
 * returns, no Lua code is executing and no coroutine is left suspended.
 *
 * @strict-properties
 * @not-serializable
 */
final class Sandbox
{
    public function __construct(?SandboxConfig $config = null) {}

    /** Version of this extension. */
    public static function extensionVersion(): string {}

    /** Version of the embedded interpreter, for example "Lua 5.5.1". */
    public static function luaVersion(): string {}

    /**
     * What this platform and this build can actually do.
     *
     * Never assume a limit is in force: on platforms with a coarse thread clock
     * the CPU limit is reported as Degraded and backed by a wall-clock deadline.
     *
     * `capabilities` answers a different question from the Capabilities object:
     * not "may this be requested" but "has anyone implemented it". A capability
     * whose subsystem is not built yet is still a valid flag that sets and reads
     * back normally, so a host granting it cannot otherwise tell that nothing
     * happened. Keys match the boolean properties of Capabilities.
     *
     * @return array{
     *     cpuLimit: LimitSupport,
     *     wallClockLimit: LimitSupport,
     *     cpuResolutionSeconds: float,
     *     threadSafe: bool,
     *     platform: string,
     *     capabilities: array<string, bool>
     * }
     */
    public static function features(): array {}

    /**
     * Compile source into a callable function without running it.
     *
     * @throws Exception\SyntaxError if the chunk does not parse.
     * @throws Exception\ClosedSandboxError if the sandbox is closed.
     */
    public function compile(string $code, string $chunkName = '=(load)'): LuaFunction {}

    /**
     * Check whether source parses, without running it and without throwing.
     *
     * The same parse compile() performs, reported as data: a host storing
     * user-authored Lua can reject a syntax error at save time and show the
     * author the line, rather than discovering it on the next run.
     *
     * An instance method rather than a static one, so the caller's limits apply
     * -- notably maxSourceBytes, whose refusal comes back as valid: false with a
     * null line, since a size limit says nothing about a position. No capability
     * is required, matching compile(): compileAtRuntime gates Lua's own load(),
     * not host-side compilation.
     *
     * Only a refusal about the SCRIPT comes back as a result -- it does not
     * parse, or it is bigger than this sandbox accepts. A closed sandbox, a
     * cross-thread call, or an interpreter that cannot grow its stack are host
     * problems rather than statements about the script, and still throw.
     *
     * Unlike compile() and eval(), a chunk name carrying neither '=' nor '@' is
     * normalised to '@name'. Lua would otherwise treat it as source text and
     * quote it as [string "..."], leaving no name to report a line against --
     * and reporting a position is this method's whole purpose.
     *
     * @throws Exception\ClosedSandboxError if the sandbox is closed.
     * @throws Exception\ThreadAffinityError if called from another thread.
     */
    #[\NoDiscard]
    public function validate(string $code, string $chunkName = '=(load)'): ValidationResult {}

    /**
     * Compile precompiled bytecode.
     *
     * Requires the loadBytecode capability. Malformed bytecode can crash the
     * interpreter, so only load blobs you produced yourself.
     *
     * @throws Exception\CapabilityError if the capability is not enabled.
     * @throws Exception\SyntaxError if the blob is not a valid chunk.
     */
    public function compileBinary(string $bytecode, string $chunkName = '=(binary)'): LuaFunction {}

    /**
     * Compile and run a chunk, returning all of its results.
     *
     * Recompiles on every call; use compile() once and reuse the LuaFunction in
     * a hot path.
     *
     * @return list<mixed>
     * @throws Exception\SyntaxError|Exception\RuntimeError|Exception\FatalError
     */
    #[\NoDiscard]
    public function eval(string $code, string $chunkName = '=(eval)'): array {}

    /**
     * Call a global function by dotted path, for example "app.handlers.main".
     *
     * @return list<mixed> every value the function returned
     * @throws Exception\RuntimeError|Exception\FatalError
     */
    #[\NoDiscard]
    public function call(string $path, mixed ...$args): array {}

    /**
     * Read a global by dotted path.
     *
     * @throws Exception\ConversionError if the value has no PHP representation.
     */
    public function getGlobal(string $path): mixed {}

    /**
     * Write a global by dotted path, creating intermediate tables.
     *
     * @throws Exception\ConversionError if the value has no Lua representation.
     */
    public function setGlobal(string $path, mixed $value): void {}

    /**
     * Expose a PHP callable to Lua.
     *
     * The callable receives converted arguments and its return value is
     * converted back. Throw a RuntimeError to raise an error the script may
     * catch; any other exception aborts the script and reaches the host intact.
     */
    public function wrapCallable(callable $callback, ?string $name = null): LuaFunction {}

    /**
     * Expose a table of PHP callables as a Lua global.
     *
     * @param array<string, callable> $functions Lua name => PHP callable
     */
    public function registerLibrary(string $name, array $functions): void {}

    /**
     * Expose an object's methods as a Lua global table of bound callables.
     *
     * Only methods carrying the LuaMethod attribute, or named in $methods, are
     * exposed; properties are never reachable and the object itself never
     * crosses into Lua.
     *
     * @param null|list<string> $methods Explicit allowlist, overriding attributes.
     * @throws Exception\ConfigurationError if neither attributes nor an allowlist select any method.
     */
    public function registerObject(string $name, object $instance, ?array $methods = null): void {}

    /**
     * Register a module so require() resolves it without consulting the
     * filesystem or the module resolver.
     */
    public function preloadModule(string $name, LuaFunction|callable $loader): void {}

    /** Null lifts the limit. */
    public function setMemoryLimit(?int $bytes): void {}

    /** Null lifts the limit. */
    public function setCpuLimit(?float $seconds): void {}

    /** Null lifts the limit. */
    public function setWallClockLimit(?float $seconds): void {}

    /**
     * Stop charging time to the script while a host callback does work of its
     * own. Only meaningful inside a callback, and a nested callback cannot
     * un-charge time its caller is already being charged for.
     *
     * @return bool whether the timers were actually paused.
     */
    public function pauseTimers(): bool {}

    public function resumeTimers(): void {}

    /**
     * Abort the running script with a HostAbortError.
     *
     * The only method safe to call from another thread.
     */
    public function interrupt(): void {}

    public function stats(): SandboxStats {}

    /** Live bytes: Lua heap plus host-side buffers charged to the sandbox. */
    public function getMemoryUsage(): int {}

    public function getPeakMemoryUsage(): int {}

    public function getCpuUsage(): float {}

    public function getWallClockUsage(): float {}

    /** Buffered output, left in place. */
    public function getOutput(): string {}

    /** Buffered output, clearing the buffer. */
    #[\NoDiscard]
    public function takeOutput(): string {}

    public function getOutputLength(): int {}

    /** Whether output was dropped after the budget was reached. */
    public function isOutputTruncated(): bool {}

    /**
     * Start sampling which Lua functions consume time.
     *
     * @return bool false when sampling is unavailable on this platform.
     */
    public function enableProfiler(float $periodSeconds = 0.002): bool {}

    public function disableProfiler(): void {}

    /**
     * Sampled cost per Lua function, most expensive first.
     *
     * @return array<string, float>
     */
    public function getProfile(ProfilerUnit $unit = ProfilerUnit::Seconds): array {}

    /**
     * Release the interpreter and everything it holds.
     *
     * Idempotent, and run automatically when the object is destroyed. Every
     * other method throws once a sandbox is closed.
     */
    public function close(): void {}

    public function isClosed(): bool {}
}

/**
 * A compiled Lua function bound to the sandbox that produced it.
 *
 * @strict-properties
 * @not-serializable
 */
final class LuaFunction
{
    /** Obtained from Sandbox::compile() and friends, never constructed directly. */
    private function __construct() {}

    /**
     * @return list<mixed> every value the function returned
     * @throws Exception\RuntimeError|Exception\FatalError|Exception\ClosedSandboxError|Exception\ThreadAffinityError
     */
    #[\NoDiscard]
    public function call(mixed ...$args): array {}

    /**
     * @return list<mixed>
     * @see LuaFunction::call()
     */
    #[\NoDiscard]
    public function __invoke(mixed ...$args): array {}

    /**
     * Serialise to precompiled bytecode.
     *
     * @throws Exception\CapabilityError if the dumpBytecode capability is not enabled.
     */
    public function dump(bool $strip = true): string {}

    public function getSandbox(): Sandbox {}

    /** False once the owning sandbox is closed. */
    public function isValid(): bool {}
}

/**
 * Metadata about a file in the virtual filesystem.
 *
 * @strict-properties
 */
final readonly class FileStat
{
    public int $size;

    /** Unix timestamp. */
    public int $mtime;

    public bool $isDirectory;

    public function __construct(int $size, int $mtime, bool $isDirectory = false) {}
}

/**
 * Backing store for the io/os file API a script sees.
 *
 * Implementations are plain blob storage: memory, Redis, a database, or a disk
 * directory. Handles, offsets, buffering and every quota are enforced inside
 * the extension, so an implementation only has to move bytes.
 *
 * Paths arrive canonicalised and absolute. Even so, never concatenate one onto
 * a host directory without validating it yourself.
 *
 * Throw a VfsError for conditions a script should be able to handle; the script
 * sees the usual `nil, message` result. Any other exception is treated as a
 * host failure, aborts the script, and reaches the caller intact.
 */
interface FileSystem
{
    public function exists(string $path): bool;

    /** Null when the path does not exist. */
    public function stat(string $path): ?FileStat;

    public function read(string $path): string;

    public function write(string $path, string $contents): void;

    public function delete(string $path): void;

    public function rename(string $from, string $to): void;

    /** @return list<string> */
    public function list(string $path): array;
}

/**
 * A filesystem that can serve byte ranges.
 *
 * Implement this when the backend can seek, and the extension will stream
 * instead of buffering whole files.
 */
interface RangedFileSystem extends FileSystem
{
    public function readRange(string $path, int $offset, int $length): string;

    public function writeRange(string $path, int $offset, string $data): void;

    public function truncate(string $path, int $size): void;
}

/**
 * Lua source returned by a module resolver.
 *
 * @strict-properties
 */
final readonly class ModuleSource
{
    public string $code;

    /** Shown in tracebacks; conventionally prefixed with `@`. */
    public string $chunkName;

    /** Requires the loadBytecode capability when true. */
    public bool $isBytecode;

    public function __construct(string $code, string $chunkName, bool $isBytecode = false) {}
}

/**
 * Resolves require() to source, after preloaded modules and VFS search paths
 * have been tried.
 */
interface ModuleResolver
{
    /**
     * @param string $module Requested module name.
     * @param string $requestedBy Chunk name of the requiring module.
     * @return ModuleSource|null Null when this resolver does not provide it.
     */
    public function resolve(string $module, string $requestedBy): ?ModuleSource;
}
