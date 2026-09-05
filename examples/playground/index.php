<?php

declare(strict_types=1);

/**
 * LuaExt web playground — a self-contained browser harness for the luaext extension.
 *
 * Run it from the repository root with the built-in PHP web server:
 *
 *     php -S localhost:8080 examples/playground/index.php
 *
 * The extension must already be loaded (it is installed via conf.d on a normal
 * `make install`; do NOT pass -d extension=… on top of that or PHP warns about a
 * double load). GET serves the UI; POST to the same URL runs one JSON action.
 *
 * SECURITY: this file refuses to serve anything except loopback clients, and
 * even then it is a development tool only. The "Host classes" panel evaluates
 * real PHP submitted from the browser — that PHP runs with the full privileges
 * of this process and is NOT sandboxed. Only the Lua side of the bridge is
 * sandboxed. Never expose this file beyond localhost.
 */

use DevelopGravity\LuaExt\Capabilities;
use DevelopGravity\LuaExt\Exception\ConfigurationError;
use DevelopGravity\LuaExt\Exception\FatalError;
use DevelopGravity\LuaExt\Exception\LuaLogicException;
use DevelopGravity\LuaExt\Exception\LuaThrowable;
use DevelopGravity\LuaExt\Exception\RuntimeError;
use DevelopGravity\LuaExt\Exception\SyntaxError;
use DevelopGravity\LuaExt\Exception\VfsError;
use DevelopGravity\LuaExt\FileStat;
use DevelopGravity\LuaExt\Limits;
use DevelopGravity\LuaExt\LuaFunction;
use DevelopGravity\LuaExt\LuaMethod;
use DevelopGravity\LuaExt\OutputMode;
use DevelopGravity\LuaExt\OverflowBehavior;
use DevelopGravity\LuaExt\ProfilerUnit;
use DevelopGravity\LuaExt\RangedFileSystem;
use DevelopGravity\LuaExt\Sandbox;
use DevelopGravity\LuaExt\SandboxConfig;
use DevelopGravity\LuaExt\VfsQuota;

// Chunk names need an @ prefix or errors lose their line numbers (compile() does
// not normalise the name the way validate() does).
const CHUNK_NAME = '@playground.lua';

const SESSION_NAME = 'LUAEXT_PLAYGROUND';

// Hard ceilings this playground enforces regardless of what the form submits.
// php -S is a single-worker server: a run that could hold an unbounded CPU or
// wall-clock budget would hang every other request behind it, so the time
// limits are never allowed to be "unlimited" here. A consequence worth knowing:
// the debugHooks capability (which requires both time limits to be unlimited)
// can therefore never be satisfied in this playground — checking it surfaces
// the extension's own ConfigurationError, which is the demo.
const MAX_RUN_SECONDS = 10.0;
const MAX_MEMORY_BYTES = 268435456; // 256 MiB
const MAX_OUTPUT_BYTES = 16777216; // 16 MiB
const MAX_VFS_FILE_BYTES = 16777216; // 16 MiB
const MAX_VFS_TOTAL_BYTES = 67108864; // 64 MiB
const MAX_PLAYGROUND_VFS_PATH_LENGTH = 255;
const MAX_DISPLAY_ARRAY_ENTRIES = 200;
const MAX_VIEW_BYTES = 262144; // 256 KiB shown by the file viewer before truncating

/**
 * How a failed request is classified for the UI (and for the HTTP status —
 * everything except Internal is an expected, renderable outcome).
 */
enum ErrorCategory
{
    case None;
    case Request;
    case Configuration;
    case Syntax;
    case HostMisuse;
    case ScriptRuntime;
    case Fatal;
    case Internal;

    public function httpStatusCode(): int
    {
        return $this === self::Internal ? 500 : 200;
    }
}

/**
 * A malformed request (bad action, bad path, invalid input JSON) — the
 * client's fault, never the sandbox's and never this file's.
 */
final class PlaygroundRequestError extends \RuntimeException
{
}

/**
 * Refuse anything that is not a loopback client, before any other work.
 *
 * REMOTE_ADDR only — forwarded headers are client-supplied and would defeat
 * the check the moment this ever sat behind a forgotten proxy.
 */
function assertRequestIsFromLocalhost(): void
{
    $remoteAddress = $_SERVER['REMOTE_ADDR'] ?? '';

    if (!in_array($remoteAddress, ['127.0.0.1', '::1'], true)) {
        http_response_code(403);
        header('Content-Type: text/plain; charset=utf-8');
        echo "Forbidden: the LuaExt playground only serves 127.0.0.1 / ::1.\n";
        exit;
    }
}

/**
 * Open the playground's session if it is not already active.
 */
function startPlaygroundSession(): void
{
    if (session_status() !== PHP_SESSION_ACTIVE) {
        session_name(SESSION_NAME);
        session_start();
    }
}

/**
 * The in-memory VFS backing the sandbox, persisted across requests in the PHP
 * session so files written by one run are visible to the next.
 *
 * Implements RangedFileSystem so the extension takes its streamed read/write
 * path rather than buffering whole files. The interface methods throw VfsError
 * for script-visible failures (the script sees Lua's `nil, message`); the
 * put/delete helpers are for the UI's direct file editing, which bypasses the
 * extension's path canonicalisation and is therefore validated separately.
 *
 * Adapted from tests/06-vfs/memory-filesystem.inc.
 */
final class PlaygroundFileSystem implements RangedFileSystem
{
    private const SESSION_KEY = 'playgroundVfsFiles';

    /** @var array<string, array{contents: string, mtime: int}> */
    private array $files = [];

    /**
     * Hydrate a filesystem from the session (empty on first visit).
     */
    public static function loadFromSession(): self
    {
        startPlaygroundSession();

        $filesystem = new self();
        $storedFiles = $_SESSION[self::SESSION_KEY] ?? [];

        if (is_array($storedFiles)) {
            foreach ($storedFiles as $path => $storedFile) {
                if (is_string($path) && is_array($storedFile) && is_string($storedFile['contents'] ?? null)) {
                    $filesystem->files[$path] = [
                        'contents' => $storedFile['contents'],
                        'mtime' => (int) ($storedFile['mtime'] ?? 0),
                    ];
                }
            }
        }

        return $filesystem;
    }

    /**
     * Persist the current file set back into the session and release the
     * session lock promptly. php -S handles one request at a time so the lock
     * is never contended here, but closing early keeps this file from becoming
     * a footgun if it is ever adapted to a concurrent SAPI.
     */
    public function saveToSession(): void
    {
        startPlaygroundSession();
        $_SESSION[self::SESSION_KEY] = $this->files;
        session_write_close();
    }

    /**
     * Direct file write from the UI (not from a running script).
     */
    public function putFile(string $path, string $contents): void
    {
        $this->files[$path] = ['contents' => $contents, 'mtime' => time()];
    }

    /**
     * Direct file delete from the UI.
     */
    public function deleteFile(string $path): void
    {
        unset($this->files[$path]);
    }

    /**
     * Raw contents for the file viewer, or null when the file does not exist.
     */
    public function fileContents(string $path): ?string
    {
        return $this->files[$path]['contents'] ?? null;
    }

    public function fileModificationTime(string $path): int
    {
        return $this->files[$path]['mtime'] ?? 0;
    }

    public function resetAllFiles(): void
    {
        $this->files = [];
    }

    /**
     * Metadata-only listing for every response (contents stay out of listings
     * so a large file cannot bloat every round-trip — the viewer fetches them
     * on demand via the vfsRead action).
     *
     * @return list<array{path: string, size: int, mtime: int, isText: bool}>
     */
    public function snapshotListing(): array
    {
        $paths = array_keys($this->files);
        sort($paths);

        $listing = [];

        foreach ($paths as $path) {
            $file = $this->files[$path];
            $listing[] = [
                'path' => $path,
                'size' => strlen($file['contents']),
                'mtime' => $file['mtime'],
                'isText' => isProbablyText(substr($file['contents'], 0, 8192)),
            ];
        }

        return $listing;
    }

    public function exists(string $path): bool
    {
        return array_key_exists($path, $this->files);
    }

    public function stat(string $path): ?FileStat
    {
        if (!array_key_exists($path, $this->files)) {
            return null;
        }

        return new FileStat(strlen($this->files[$path]['contents']), $this->files[$path]['mtime']);
    }

    public function read(string $path): string
    {
        return $this->files[$path]['contents']
            ?? throw new VfsError(sprintf('no such file: %s', $path));
    }

    public function write(string $path, string $contents): void
    {
        $this->files[$path] = ['contents' => $contents, 'mtime' => time()];
    }

    public function delete(string $path): void
    {
        if (!array_key_exists($path, $this->files)) {
            throw new VfsError(sprintf('no such file: %s', $path));
        }

        unset($this->files[$path]);
    }

    public function rename(string $from, string $to): void
    {
        if (!array_key_exists($from, $this->files)) {
            throw new VfsError(sprintf('no such file: %s', $from));
        }

        $this->files[$to] = $this->files[$from];
        $this->files[$to]['mtime'] = time();
        unset($this->files[$from]);
    }

    /**
     * @return list<string>
     */
    public function list(string $path): array
    {
        $prefix = rtrim($path, '/') . '/';
        $names = [];

        foreach (array_keys($this->files) as $candidate) {
            if (str_starts_with($candidate, $prefix)) {
                $remainder = substr($candidate, strlen($prefix));

                // Direct children only, the way a directory listing works.
                if (!str_contains($remainder, '/')) {
                    $names[] = $remainder;
                }
            }
        }

        sort($names);

        return $names;
    }

    public function readRange(string $path, int $offset, int $length): string
    {
        if (!array_key_exists($path, $this->files)) {
            throw new VfsError(sprintf('no such file: %s', $path));
        }

        return substr($this->files[$path]['contents'], $offset, $length);
    }

    public function writeRange(string $path, int $offset, string $data): void
    {
        $existing = $this->files[$path]['contents'] ?? '';

        // A write past the end zero-fills the gap, as a real sparse file does.
        if ($offset > strlen($existing)) {
            $existing .= str_repeat("\0", $offset - strlen($existing));
        }

        $this->files[$path] = [
            'contents' => substr_replace($existing, $data, $offset, strlen($data)),
            'mtime' => time(),
        ];
    }

    public function truncate(string $path, int $size): void
    {
        $existing = $this->files[$path]['contents'] ?? '';

        $this->files[$path] = [
            'contents' => $size <= strlen($existing)
                ? substr($existing, 0, $size)
                : $existing . str_repeat("\0", $size - strlen($existing)),
            'mtime' => time(),
        ];
    }
}

/**
 * Whether a byte string is sensibly displayable as text: no NUL bytes, valid
 * UTF-8, and no C0 control characters other than tab/newline/carriage return.
 * (A truncated sample can split a multi-byte sequence and misread text as
 * binary; that only downgrades the display, never corrupts anything.)
 */
function isProbablyText(string $bytes): bool
{
    if ($bytes === '') {
        return true;
    }

    if (str_contains($bytes, "\0") || !mb_check_encoding($bytes, 'UTF-8')) {
        return false;
    }

    return preg_match('/[\x01-\x08\x0B\x0C\x0E-\x1F]/', $bytes) !== 1;
}

/**
 * xxd-style hex dump: 8-digit offset, 16 bytes per row as 2-byte hex groups,
 * ASCII gutter with '.' standing in for non-printable bytes.
 */
function formatHexDump(string $bytes): string
{
    $rows = [];
    $totalLength = strlen($bytes);

    for ($offset = 0; $offset < $totalLength; $offset += 16) {
        $slice = substr($bytes, $offset, 16);
        $hexGroups = str_split(bin2hex($slice), 4);
        $hexColumn = str_pad(implode(' ', $hexGroups), 39);
        $asciiColumn = preg_replace('/[^\x20-\x7E]/', '.', $slice) ?? '';
        $rows[] = sprintf('%08x: %s  %s', $offset, $hexColumn, $asciiColumn);
    }

    return implode("\n", $rows);
}

/**
 * Read a boolean field from a request payload.
 *
 * @param array<string, mixed> $source
 */
function readBool(array $source, string $key, bool $default): bool
{
    return is_bool($source[$key] ?? null) ? $source[$key] : $default;
}

/**
 * Read an integer field from a request payload, clamped to a sane range.
 *
 * @param array<string, mixed> $source
 */
function readClampedInt(array $source, string $key, int $default, int $minimum, int $maximum): int
{
    $raw = $source[$key] ?? null;

    if (!is_numeric($raw)) {
        return $default;
    }

    return max($minimum, min($maximum, (int) $raw));
}

/**
 * Read a float field from a request payload, clamped to a sane range.
 *
 * @param array<string, mixed> $source
 */
function readClampedFloat(array $source, string $key, float $default, float $minimum, float $maximum): float
{
    $raw = $source[$key] ?? null;

    if (!is_numeric($raw) || !is_finite((float) $raw)) {
        return $default;
    }

    return max($minimum, min($maximum, (float) $raw));
}

/**
 * Resolve a CPU/wall-clock limit. Never returns unlimited: null, zero, and
 * junk all become the playground cap (see the MAX_RUN_SECONDS comment).
 *
 * @param array<string, mixed> $rawLimits
 */
function resolveSecondsLimit(array $rawLimits, string $key, float $default): float
{
    $requested = $rawLimits[$key] ?? $default;

    if (!is_numeric($requested) || !is_finite((float) $requested) || (float) $requested <= 0.0) {
        return MAX_RUN_SECONDS;
    }

    return min((float) $requested, MAX_RUN_SECONDS);
}

/**
 * Resolve the memory limit. The API treats null/0 as "unlimited"; both are
 * clamped to the playground ceiling here, and a literal 0 must never slip
 * through as a zero-byte allowance.
 *
 * @param array<string, mixed> $rawLimits
 */
function resolveMemoryBytesLimit(array $rawLimits): int
{
    $requested = $rawLimits['memoryBytes'] ?? null;

    if (!is_numeric($requested) || (int) $requested <= 0) {
        return MAX_MEMORY_BYTES;
    }

    return min((int) $requested, MAX_MEMORY_BYTES);
}

/**
 * Decode the "input" textarea: empty means no input, anything else must be
 * valid JSON (decoded to associative arrays — objects would be refused by the
 * PHP-to-Lua conversion layer).
 */
function decodeInputJsonOrThrow(string $inputJson): mixed
{
    if (trim($inputJson) === '') {
        return null;
    }

    try {
        return json_decode($inputJson, associative: true, flags: JSON_THROW_ON_ERROR);
    } catch (\JsonException $jsonException) {
        throw new PlaygroundRequestError('the input JSON is invalid: ' . $jsonException->getMessage());
    }
}

/**
 * @param array<string, mixed> $payload
 */
function resolveOutputMode(array $payload): OutputMode
{
    return match ($payload['outputMode'] ?? 'Buffer') {
        'Callback' => OutputMode::Callback,
        'Discard' => OutputMode::Discard,
        default => OutputMode::Buffer,
    };
}

/**
 * @param array<string, mixed> $payload
 */
function resolveProfilerUnit(array $payload): ProfilerUnit
{
    return match ($payload['profilerUnit'] ?? 'Seconds') {
        'Samples' => ProfilerUnit::Samples,
        'Percent' => ProfilerUnit::Percent,
        default => ProfilerUnit::Seconds,
    };
}

/**
 * @param array<string, mixed> $rawLimits
 */
function resolveOverflowBehavior(array $rawLimits): OverflowBehavior
{
    return ($rawLimits['outputOverflow'] ?? 'Fail') === 'Truncate'
        ? OverflowBehavior::Truncate
        : OverflowBehavior::Fail;
}

/**
 * Register the built-in json demo library: two callables bridging to ext-json.
 * Failures are rethrown as RuntimeError so the script can pcall them.
 */
function registerJsonLibrary(Sandbox $sandbox): void
{
    $sandbox->registerLibrary('json', [
        'encode' => static function (mixed $value): string {
            try {
                return json_encode($value, JSON_THROW_ON_ERROR | JSON_UNESCAPED_SLASHES);
            } catch (\JsonException $jsonException) {
                throw new RuntimeError('json.encode: ' . $jsonException->getMessage());
            }
        },
        'decode' => static function (string $text): mixed {
            try {
                return json_decode($text, associative: true, flags: JSON_THROW_ON_ERROR);
            } catch (\JsonException $jsonException) {
                throw new RuntimeError('json.decode: ' . $jsonException->getMessage());
            }
        },
    ]);
}

/**
 * Parse an HTML string with DOMDocument, with libxml warnings silenced.
 */
function parsePlaygroundHtmlDocument(string $htmlSource): \DOMDocument
{
    if (trim($htmlSource) === '') {
        throw new RuntimeError('html: the document is empty');
    }

    $document = new \DOMDocument();
    $previousUseInternalErrors = libxml_use_internal_errors(true);

    try {
        if (!$document->loadHTML($htmlSource, LIBXML_NOERROR | LIBXML_NOWARNING)) {
            throw new RuntimeError('html: the document could not be parsed');
        }
    } finally {
        libxml_clear_errors();
        libxml_use_internal_errors($previousUseInternalErrors);
    }

    return $document;
}

/**
 * Register the built-in html demo library backed by DOMDocument.
 * links() re-indexes to a 1-based sequence — the raw PHP list would arrive in
 * Lua with its first element at key 0, invisible to # and ipairs.
 */
function registerHtmlLibrary(Sandbox $sandbox): void
{
    $sandbox->registerLibrary('html', [
        'title' => static function (string $htmlSource): ?string {
            $titles = parsePlaygroundHtmlDocument($htmlSource)->getElementsByTagName('title');

            return $titles->length > 0 ? trim($titles->item(0)->textContent) : null;
        },
        'text' => static function (string $htmlSource): string {
            $body = parsePlaygroundHtmlDocument($htmlSource)->getElementsByTagName('body')->item(0);

            return trim(preg_replace('/\s+/', ' ', $body?->textContent ?? '') ?? '');
        },
        'links' => static function (string $htmlSource): array {
            $hrefs = [];

            foreach (parsePlaygroundHtmlDocument($htmlSource)->getElementsByTagName('a') as $anchor) {
                $href = $anchor->getAttribute('href');

                if ($href !== '') {
                    $hrefs[] = $href;
                }
            }

            return $hrefs === [] ? [] : array_combine(range(1, count($hrefs)), $hrefs);
        },
    ]);
}

/**
 * The registerObject / #[LuaMethod] demo: a stateful counter and key-value
 * store whose one instance backs every Lua-to-PHP call inside a single run.
 * A fresh instance is created per request, so unlike the VFS this state does
 * not persist across runs — an intentional, teachable contrast.
 */
final class PlaygroundToolkit
{
    private int $counter = 0;

    /** @var array<string, mixed> */
    private array $rememberedValues = [];

    /**
     * Add $step to the counter and return the new value.
     */
    #[LuaMethod('increment')]
    public function incrementCounter(int $step = 1): int
    {
        return $this->counter += $step;
    }

    /**
     * Current counter value.
     */
    #[LuaMethod('value')]
    public function counterValue(): int
    {
        return $this->counter;
    }

    /**
     * Reset the counter and the key-value store.
     */
    #[LuaMethod('reset')]
    public function resetState(): bool
    {
        $this->counter = 0;
        $this->rememberedValues = [];

        return true;
    }

    /**
     * Store a value under a key.
     */
    #[LuaMethod('remember')]
    public function rememberValue(string $key, mixed $value): bool
    {
        if ($key === '') {
            throw new RuntimeError('toolkit.remember: the key must not be empty');
        }

        $this->rememberedValues[$key] = $value;

        return true;
    }

    /**
     * Recall a stored value (nil when absent).
     */
    #[LuaMethod('recall')]
    public function recallValue(string $key): mixed
    {
        return $this->rememberedValues[$key] ?? null;
    }
}

/**
 * Register the user-defined host classes from the UI panel.
 *
 * Each snippet is eval()'d — deliberately unsandboxed host PHP (see the file
 * header) — and must return either an object (registered via registerObject,
 * #[LuaMethod]-marked methods become visible) or a non-empty map of named
 * callables (registered via registerLibrary). Every failure is collected as a
 * per-entry error rather than aborting the run.
 *
 * @param list<mixed> $entries
 * @return array{errors: list<array{luaName: ?string, message: string}>, warnings: list<string>}
 */
function registerUserDefinedHostClasses(Sandbox $sandbox, array $entries): array
{
    $errors = [];
    $warnings = [];

    foreach ($entries as $entry) {
        $luaName = is_array($entry) && is_string($entry['luaName'] ?? null) ? trim($entry['luaName']) : '';
        $phpSource = is_array($entry) && is_string($entry['phpSource'] ?? null) ? $entry['phpSource'] : '';

        if ($luaName === '' && trim($phpSource) === '') {
            continue; // an empty editor row, not an error
        }

        if (preg_match('/^[A-Za-z_][A-Za-z0-9_]*$/', $luaName) !== 1) {
            $errors[] = [
                'luaName' => $luaName === '' ? null : $luaName,
                'message' => 'the Lua name must be a valid identifier (letters, digits, underscores)',
            ];
            continue;
        }

        if (in_array($luaName, ['json', 'html', 'toolkit'], true)) {
            $warnings[] = sprintf('host class "%s" collides with a built-in demo library of the same name', $luaName);
        }

        try {
            $normalizedSource = preg_replace('/^\s*<\?php\s+/', '', $phpSource) ?? $phpSource;
            $evaluated = eval($normalizedSource);

            if (is_object($evaluated)) {
                $sandbox->registerObject($luaName, $evaluated);
            } elseif (
                is_array($evaluated)
                && $evaluated !== []
                && array_all($evaluated, static fn (mixed $callable, mixed $name): bool => is_string($name) && is_callable($callable))
            ) {
                $sandbox->registerLibrary($luaName, $evaluated);
            } else {
                throw new PlaygroundRequestError(
                    'the snippet must return an object or a non-empty array<string, callable>'
                );
            }
        } catch (\Throwable $registrationError) {
            $errors[] = ['luaName' => $luaName, 'message' => $registrationError->getMessage()];
        }
    }

    return ['errors' => $errors, 'warnings' => $warnings];
}

/**
 * Make an already-converted Lua value safe to json_encode for display.
 *
 * The extension has done the dangerous work (depth caps, cycle refusal, key
 * collision refusal) before the value ever reaches PHP, so this layer only
 * handles display-safety: LuaFunction handles, binary strings, non-finite
 * floats, and very wide tables. Tables always render as a tagged entry list —
 * never re-indexed — so a PHP list's key 0 (which sits outside Lua's #
 * sequence) stays visible instead of being papered over.
 */
function presentLuaValueForDisplay(mixed $value): mixed
{
    return match (true) {
        $value === null, is_bool($value), is_int($value) => $value,
        is_float($value) => is_finite($value)
            ? $value
            : ['__kind' => 'nonFiniteFloat', 'value' => is_nan($value) ? 'nan' : ($value > 0 ? 'inf' : '-inf')],
        is_string($value) => presentBytesForDisplay($value),
        $value instanceof LuaFunction => ['__kind' => 'luaFunction', 'valid' => $value->isValid()],
        is_array($value) => presentTableForDisplay($value),
        default => ['__kind' => 'unrepresentable', 'phpType' => get_debug_type($value)],
    };
}

/**
 * A string passes through when valid UTF-8; anything else becomes a tagged
 * base64 wrapper (json_encode would otherwise fail on it).
 *
 * @return string|array{__kind: 'binaryString', base64: string, length: int}
 */
function presentBytesForDisplay(string $bytes): string|array
{
    if (mb_check_encoding($bytes, 'UTF-8')) {
        return $bytes;
    }

    return ['__kind' => 'binaryString', 'base64' => base64_encode($bytes), 'length' => strlen($bytes)];
}

/**
 * @param array<int|string, mixed> $table
 * @return array{__kind: 'table', entries: list<array{key: int|string|null, keyType: string, value: mixed}>}
 */
function presentTableForDisplay(array $table): array
{
    $entries = [];
    $shown = 0;

    foreach ($table as $key => $value) {
        if (++$shown > MAX_DISPLAY_ARRAY_ENTRIES) {
            $entries[] = [
                'key' => null,
                'keyType' => 'truncated',
                'value' => sprintf('%d more entries omitted', count($table) - MAX_DISPLAY_ARRAY_ENTRIES),
            ];
            break;
        }

        $entries[] = [
            'key' => is_string($key) ? presentBytesForDisplay($key) : $key,
            'keyType' => is_int($key) ? 'integer' : 'string',
            'value' => presentLuaValueForDisplay($value),
        ];
    }

    return ['__kind' => 'table', 'entries' => $entries];
}

/**
 * Build Capabilities from the request. Full constructor with every named
 * argument (never ->with(), whose unknown-name behaviour is to throw) so a
 * missing key falls back to the extension's own documented default.
 *
 * @param array<string, mixed> $payload
 */
function buildCapabilitiesFromPayload(array $payload): Capabilities
{
    $rawCapabilities = is_array($payload['capabilities'] ?? null) ? $payload['capabilities'] : [];

    $rawAllowList = $rawCapabilities['osEnvAllowList'] ?? [];

    if (is_string($rawAllowList)) {
        $rawAllowList = explode(',', $rawAllowList);
    }

    $osEnvAllowList = array_values(array_filter(
        array_map(static fn (mixed $name): string => trim((string) $name), is_array($rawAllowList) ? $rawAllowList : []),
        static fn (string $name): bool => $name !== '',
    ));

    return new Capabilities(
        loadBytecode: readBool($rawCapabilities, 'loadBytecode', false),
        compileAtRuntime: readBool($rawCapabilities, 'compileAtRuntime', false),
        dumpBytecode: readBool($rawCapabilities, 'dumpBytecode', false),
        require: readBool($rawCapabilities, 'require', false),
        vfs: readBool($rawCapabilities, 'vfs', false),
        vfsWrite: readBool($rawCapabilities, 'vfsWrite', false),
        coroutines: readBool($rawCapabilities, 'coroutines', true),
        osTime: readBool($rawCapabilities, 'osTime', true),
        osEnv: readBool($rawCapabilities, 'osEnv', false),
        osEnvAllowList: $osEnvAllowList,
        debugTraceback: readBool($rawCapabilities, 'debugTraceback', true),
        debugIntrospect: readBool($rawCapabilities, 'debugIntrospect', false),
        debugMutate: readBool($rawCapabilities, 'debugMutate', false),
        debugHooks: readBool($rawCapabilities, 'debugHooks', false),
        utf8: readBool($rawCapabilities, 'utf8', true),
        gcControl: readBool($rawCapabilities, 'gcControl', false),
        warn: readBool($rawCapabilities, 'warn', false),
    );
}

/**
 * @param array<string, mixed> $payload
 */
function buildLimitsFromPayload(array $payload): Limits
{
    $rawLimits = is_array($payload['limits'] ?? null) ? $payload['limits'] : [];

    return new Limits(
        memoryBytes: resolveMemoryBytesLimit($rawLimits),
        cpuSeconds: resolveSecondsLimit($rawLimits, 'cpuSeconds', 1.0),
        wallClockSeconds: resolveSecondsLimit($rawLimits, 'wallClockSeconds', 5.0),
        outputBytes: readClampedInt($rawLimits, 'outputBytes', 1048576, 1024, MAX_OUTPUT_BYTES),
        outputOverflow: resolveOverflowBehavior($rawLimits),
        maxLiveCoroutines: readClampedInt($rawLimits, 'maxLiveCoroutines', 64, 1, 1024),
        maxCoroutineDepth: readClampedInt($rawLimits, 'maxCoroutineDepth', 16, 1, 256),
        maxCallDepth: readClampedInt($rawLimits, 'maxCallDepth', 200, 1, 5000),
        maxModules: readClampedInt($rawLimits, 'maxModules', 64, 1, 1024),
        maxRequireDepth: readClampedInt($rawLimits, 'maxRequireDepth', 16, 1, 64),
        maxStringLength: readClampedInt($rawLimits, 'maxStringLength', 67108864, 1024, MAX_MEMORY_BYTES),
        maxSourceBytes: readClampedInt($rawLimits, 'maxSourceBytes', 1048576, 1024, MAX_VFS_FILE_BYTES),
        maxConversionDepth: readClampedInt($rawLimits, 'maxConversionDepth', 64, 1, 256),
        maxCachedChunks: readClampedInt($rawLimits, 'maxCachedChunks', 64, 1, 1024),
    );
}

/**
 * @param array<string, mixed> $payload
 */
function buildVfsQuotaFromPayload(array $payload): VfsQuota
{
    $rawQuota = is_array($payload['vfsQuota'] ?? null) ? $payload['vfsQuota'] : [];

    return new VfsQuota(
        maxOpenHandles: readClampedInt($rawQuota, 'maxOpenHandles', 16, 1, 256),
        maxFileBytes: readClampedInt($rawQuota, 'maxFileBytes', 1048576, 1, MAX_VFS_FILE_BYTES),
        maxTotalBytes: readClampedInt($rawQuota, 'maxTotalBytes', 8388608, 1, MAX_VFS_TOTAL_BYTES),
        maxFiles: readClampedInt($rawQuota, 'maxFiles', 128, 1, 4096),
        maxOperations: readClampedInt($rawQuota, 'maxOperations', 10000, 1, 1000000),
        maxPathLength: readClampedInt($rawQuota, 'maxPathLength', 255, 1, 1024),
        maxPathDepth: readClampedInt($rawQuota, 'maxPathDepth', 16, 1, 64),
        billWallTime: readBool($rawQuota, 'billWallTime', false),
    );
}

/**
 * Assemble the SandboxConfig. The filesystem is always supplied (harmless when
 * the vfs capability is off, and it lets the visitor toggle vfs freely); the
 * seed is only forwarded together with deterministic, matching the API rule.
 *
 * @param array<string, mixed> $payload
 */
function buildSandboxConfigFromPayload(
    array $payload,
    PlaygroundFileSystem $filesystem,
    OutputMode $outputMode,
    ?\Closure $outputCallback,
): SandboxConfig {
    $deterministic = readBool($payload, 'deterministic', false);
    $rawSeed = $payload['seed'] ?? null;

    return new SandboxConfig(
        capabilities: buildCapabilitiesFromPayload($payload),
        limits: buildLimitsFromPayload($payload),
        filesystem: $filesystem,
        vfsQuota: buildVfsQuotaFromPayload($payload),
        outputMode: $outputMode,
        outputCallback: $outputMode === OutputMode::Callback ? $outputCallback : null,
        outputChunkBytes: readClampedInt($payload, 'outputChunkBytes', 8192, 1, 1048576),
        seed: $deterministic && is_numeric($rawSeed) ? (int) $rawSeed : null,
        deterministic: $deterministic,
        cacheCompiledChunks: readBool($payload, 'cacheCompiledChunks', false),
    );
}

/**
 * Classify a failure. The instanceof order is load-bearing: SyntaxError
 * extends FatalError, ConfigurationError extends LuaLogicException.
 */
function classifyError(\Throwable $error): ErrorCategory
{
    return match (true) {
        $error instanceof PlaygroundRequestError => ErrorCategory::Request,
        $error instanceof ConfigurationError => ErrorCategory::Configuration,
        $error instanceof SyntaxError => ErrorCategory::Syntax,
        $error instanceof LuaLogicException => ErrorCategory::HostMisuse,
        $error instanceof RuntimeError => ErrorCategory::ScriptRuntime,
        $error instanceof FatalError => ErrorCategory::Fatal,
        default => ErrorCategory::Internal,
    };
}

/**
 * The error fields shared by every failing response. Lua-side details are only
 * present when the exception actually crossed the sandbox boundary.
 *
 * @return array<string, mixed>
 */
function presentErrorForResponse(\Throwable $error, ErrorCategory $category): array
{
    $fields = [
        'errorCategory' => $category->name,
        'errorClass' => $error::class,
        'message' => $error->getMessage(),
    ];

    if ($error instanceof LuaThrowable) {
        $fields['luaLine'] = $error->getLuaLine();
        $fields['chunkName'] = $error->getChunkName();
        $fields['luaTrace'] = $error->getLuaTrace();
        $fields['luaTraceFormatted'] = $error->getLuaTraceAsString();
    }

    if ($category === ErrorCategory::Internal) {
        // Loopback-only tool: a full trace in the browser is a feature here.
        $fields['trace'] = $error->getTraceAsString();
    }

    return $fields;
}

/**
 * Execute one Lua run and gather every observable piece of it.
 *
 * One broad catch, classified afterwards — ConfigurationError can be thrown by
 * either the SandboxConfig constructor or new Sandbox() depending on which
 * check fires, so pinning catches to lines would be fragile.
 *
 * @param array<string, mixed> $payload
 * @return array{
 *   error: ?\Throwable,
 *   returnValues: ?list<mixed>,
 *   stats: ?object,
 *   output: string,
 *   callbackChunks: list<array{chunk: string, isStderr: bool}>,
 *   profile: ?array<string, float>,
 *   profilerUnit: ProfilerUnit,
 *   hostClassErrors: list<array{luaName: ?string, message: string}>,
 *   warnings: list<string>
 * }
 */
function runSandboxPipeline(array $payload, PlaygroundFileSystem $filesystem): array
{
    $sandbox = null;
    $error = null;
    $returnValues = null;
    $stats = null;
    $capturedOutput = '';
    $callbackChunks = [];
    $profileEntries = null;
    $profilerActive = false;
    $hostClassErrors = [];
    $warnings = [];

    $outputMode = resolveOutputMode($payload);
    $profilerRequested = readBool($payload, 'profilerEnabled', false);
    $profilerUnit = resolveProfilerUnit($payload);

    try {
        $decodedInput = decodeInputJsonOrThrow((string) ($payload['inputJson'] ?? ''));

        $outputCallback = static function (string $chunk, bool $isStandardError) use (&$callbackChunks): void {
            $callbackChunks[] = ['chunk' => $chunk, 'isStderr' => $isStandardError];
        };

        $config = buildSandboxConfigFromPayload($payload, $filesystem, $outputMode, $outputCallback);
        $sandbox = new Sandbox($config);

        registerJsonLibrary($sandbox);
        registerHtmlLibrary($sandbox);
        $sandbox->registerObject('toolkit', new PlaygroundToolkit());

        $userClassEntries = is_array($payload['hostClasses'] ?? null) ? $payload['hostClasses'] : [];
        $registration = registerUserDefinedHostClasses($sandbox, $userClassEntries);
        $hostClassErrors = $registration['errors'];
        $warnings = [...$warnings, ...$registration['warnings']];

        // The input is available both ways: as the global `input` and as the
        // chunk's first vararg (`local input = ...`).
        $sandbox->setGlobal('input', $decodedInput);

        if ($profilerRequested) {
            $profilerActive = $sandbox->enableProfiler(
                readClampedFloat($payload, 'profilerPeriodSeconds', 0.002, 0.0001, 1.0)
            );

            if (!$profilerActive) {
                $warnings[] = 'The sampling profiler is unsupported on this platform; no profile was collected.';
            }
        }

        $compiledChunk = $sandbox->compile((string) ($payload['source'] ?? ''), CHUNK_NAME);
        $rawReturnValues = $compiledChunk->call($decodedInput);

        // Present before close(): LuaFunction handles die with the sandbox, and
        // presenting live ones keeps their isValid() badge truthful.
        $returnValues = array_map(presentLuaValueForDisplay(...), $rawReturnValues);
    } catch (\Throwable $caught) {
        $error = $caught;
    } finally {
        if ($sandbox !== null) {
            // Best effort: after a PanicError the sandbox refuses further use,
            // and a secondary failure here must not mask the primary one.
            try {
                if (!$sandbox->isClosed()) {
                    if ($profilerActive) {
                        $sandbox->disableProfiler();
                        $profileEntries = $sandbox->getProfile($profilerUnit);
                    }

                    $stats = $sandbox->stats();
                    $capturedOutput = $outputMode === OutputMode::Buffer ? $sandbox->takeOutput() : '';
                    $sandbox->close();
                }
            } catch (\Throwable) {
                // Keep whatever was already gathered.
            }
        }
    }

    return [
        'error' => $error,
        'returnValues' => $returnValues,
        'stats' => $stats,
        'output' => $capturedOutput,
        'callbackChunks' => $callbackChunks,
        'profile' => $profileEntries,
        'profilerUnit' => $profilerUnit,
        'hostClassErrors' => $hostClassErrors,
        'warnings' => $warnings,
    ];
}

/**
 * Validate a path submitted directly by the UI. These bypass the extension's
 * own canonicalisation (that guarantee only covers paths the C VFS layer hands
 * to the backend during a run), so they get their own gate.
 */
function assertValidPlaygroundVfsPath(string $path): void
{
    $isWellFormed = strlen($path) <= MAX_PLAYGROUND_VFS_PATH_LENGTH
        && !str_contains($path, "\0")
        && preg_match('#^(/[^/\0]+)+$#', $path) === 1
        && !in_array('..', explode('/', $path), true)
        && !in_array('.', explode('/', $path), true);

    if (!$isWellFormed) {
        throw new PlaygroundRequestError(sprintf(
            'invalid VFS path "%s": paths are absolute, /-separated, without . or .. segments, at most %d bytes',
            $path,
            MAX_PLAYGROUND_VFS_PATH_LENGTH,
        ));
    }
}

/**
 * Run action: execute the source and report everything.
 *
 * @param array<string, mixed> $payload
 * @return array<string, mixed>
 */
function handleRunAction(array $payload, PlaygroundFileSystem $filesystem): array
{
    $pipeline = runSandboxPipeline($payload, $filesystem);
    $error = $pipeline['error'];

    $response = [
        'ok' => $error === null,
        'action' => 'run',
        'output' => $pipeline['output'],
        'callbackChunks' => $pipeline['callbackChunks'],
        'stats' => $pipeline['stats'],
        'profile' => $pipeline['profile'] === null
            ? null
            : ['unit' => $pipeline['profilerUnit']->name, 'entries' => $pipeline['profile']],
        'hostClassErrors' => $pipeline['hostClassErrors'],
        'warnings' => $pipeline['warnings'],
        'meta' => ['chunkName' => ltrim(CHUNK_NAME, '@=')],
    ];

    if ($error === null) {
        $response['returnValues'] = $pipeline['returnValues'] ?? [];
    } else {
        $response += presentErrorForResponse($error, classifyError($error));
    }

    return $response;
}

/**
 * Validate action: syntax-check only. A bad script is data (ok stays true);
 * only a host-side failure while building the sandbox makes this fail.
 *
 * @param array<string, mixed> $payload
 * @return array<string, mixed>
 */
function handleValidateAction(array $payload, PlaygroundFileSystem $filesystem): array
{
    $sandbox = null;

    try {
        $config = buildSandboxConfigFromPayload($payload, $filesystem, OutputMode::Buffer, null);
        $sandbox = new Sandbox($config);
        $validationResult = $sandbox->validate((string) ($payload['source'] ?? ''), CHUNK_NAME);

        return ['ok' => true, 'action' => 'validate'] + $validationResult->jsonSerialize();
    } catch (\Throwable $caught) {
        return ['ok' => false, 'action' => 'validate'] + presentErrorForResponse($caught, classifyError($caught));
    } finally {
        if ($sandbox !== null && !$sandbox->isClosed()) {
            $sandbox->close();
        }
    }
}

/**
 * @param array<string, mixed> $payload
 * @return array<string, mixed>
 */
function handleVfsPutAction(array $payload, PlaygroundFileSystem $filesystem): array
{
    $path = trim((string) ($payload['path'] ?? ''));
    assertValidPlaygroundVfsPath($path);

    $content = (string) ($payload['content'] ?? '');

    if (strlen($content) > MAX_VFS_FILE_BYTES) {
        throw new PlaygroundRequestError(sprintf('the file exceeds the %d byte editor cap', MAX_VFS_FILE_BYTES));
    }

    $filesystem->putFile($path, $content);

    return ['ok' => true, 'action' => 'vfsPut', 'path' => $path];
}

/**
 * @param array<string, mixed> $payload
 * @return array<string, mixed>
 */
function handleVfsDeleteAction(array $payload, PlaygroundFileSystem $filesystem): array
{
    $path = trim((string) ($payload['path'] ?? ''));
    assertValidPlaygroundVfsPath($path);
    $filesystem->deleteFile($path);

    return ['ok' => true, 'action' => 'vfsDelete', 'path' => $path];
}

/**
 * File viewer: text files come back as plain text, binary files as an
 * xxd-style hex dump, both capped at MAX_VIEW_BYTES.
 *
 * @param array<string, mixed> $payload
 * @return array<string, mixed>
 */
function handleVfsReadAction(array $payload, PlaygroundFileSystem $filesystem): array
{
    $path = trim((string) ($payload['path'] ?? ''));
    assertValidPlaygroundVfsPath($path);

    $contents = $filesystem->fileContents($path);

    if ($contents === null) {
        throw new PlaygroundRequestError(sprintf('no such file: %s', $path));
    }

    $viewSlice = substr($contents, 0, MAX_VIEW_BYTES);
    $truncated = strlen($contents) > MAX_VIEW_BYTES;

    $response = [
        'ok' => true,
        'action' => 'vfsRead',
        'path' => $path,
        'size' => strlen($contents),
        'mtime' => $filesystem->fileModificationTime($path),
        'truncated' => $truncated,
    ];

    if (isProbablyText($viewSlice)) {
        $response['kind'] = 'text';
        $response['content'] = $viewSlice;
    } else {
        $response['kind'] = 'binary';
        $response['hexDump'] = formatHexDump($viewSlice);
    }

    return $response;
}

/**
 * @return array<string, mixed>
 */
function handleResetVfsAction(PlaygroundFileSystem $filesystem): array
{
    $filesystem->resetAllFiles();

    return ['ok' => true, 'action' => 'resetVfs'];
}

/**
 * Sandbox::features() carries LimitSupport enum instances, which json_encode
 * refuses; flatten them to their names.
 *
 * @return array<string, mixed>
 */
function presentFeaturesForDisplay(): array
{
    $features = Sandbox::features();

    return [
        'cpuLimit' => $features['cpuLimit']->name,
        'wallClockLimit' => $features['wallClockLimit']->name,
        'cpuResolutionSeconds' => $features['cpuResolutionSeconds'],
        'threadSafe' => $features['threadSafe'],
        'platform' => $features['platform'],
        'capabilities' => $features['capabilities'],
    ];
}

/**
 * Emit a JSON response, with a last-resort fallback if encoding itself fails
 * (a silently partial body would be worse than an honest 500).
 *
 * @param array<string, mixed> $response
 */
function emitJsonResponse(int $statusCode, array $response): void
{
    $encodedResponse = json_encode($response, JSON_UNESCAPED_SLASHES | JSON_INVALID_UTF8_SUBSTITUTE);

    if ($encodedResponse === false) {
        $statusCode = 500;
        $encodedResponse = '{"ok":false,"errorCategory":"Internal","message":"the response could not be encoded as JSON"}';
    }

    http_response_code($statusCode);
    header('Content-Type: application/json; charset=utf-8');
    echo $encodedResponse;
}

/**
 * Decode the POST body, dispatch the action, persist the VFS (even after a
 * failure — writes a script made before dying should stick), attach the
 * current VFS listing, and emit.
 */
function handlePostRequest(): void
{
    $rawBody = (string) file_get_contents('php://input');

    try {
        $payload = json_decode($rawBody, associative: true, flags: JSON_THROW_ON_ERROR);

        if (!is_array($payload)) {
            throw new \JsonException('the body must be a JSON object');
        }
    } catch (\JsonException $jsonException) {
        emitJsonResponse(200, [
            'ok' => false,
            'action' => null,
            'errorCategory' => ErrorCategory::Request->name,
            'message' => 'the request body is not a JSON object: ' . $jsonException->getMessage(),
            'vfsState' => [],
        ]);

        return;
    }

    $action = is_string($payload['action'] ?? null) ? $payload['action'] : '';
    $filesystem = PlaygroundFileSystem::loadFromSession();

    try {
        $response = match ($action) {
            'run' => handleRunAction($payload, $filesystem),
            'validate' => handleValidateAction($payload, $filesystem),
            'vfsPut' => handleVfsPutAction($payload, $filesystem),
            'vfsDelete' => handleVfsDeleteAction($payload, $filesystem),
            'vfsRead' => handleVfsReadAction($payload, $filesystem),
            'resetVfs' => handleResetVfsAction($filesystem),
            default => throw new PlaygroundRequestError(sprintf('unknown action "%s"', $action)),
        };
    } catch (PlaygroundRequestError $requestError) {
        $response = [
            'ok' => false,
            'action' => $action,
            'errorCategory' => ErrorCategory::Request->name,
            'message' => $requestError->getMessage(),
        ];
    } catch (\Throwable $internalError) {
        $response = ['ok' => false, 'action' => $action]
            + presentErrorForResponse($internalError, ErrorCategory::Internal);
    } finally {
        $filesystem->saveToSession();
    }

    $response['vfsState'] = $filesystem->snapshotListing();

    $statusCode = ($response['errorCategory'] ?? null) === ErrorCategory::Internal->name ? 500 : 200;
    emitJsonResponse($statusCode, $response);
}

/**
 * The demo scripts offered by the preset dropdown. requiredCapabilities are
 * auto-checked client-side; seedFiles are vfsPut client-side when absent.
 */
const PRESET_EXAMPLES = [
    'hello' => [
        'label' => 'Hello world',
        'description' => 'print(), multiple return values, and the stats panel.',
        'requiredCapabilities' => [],
        'seedFiles' => [],
        'lua' => <<<'LUA'
            print("Hello from " .. _VERSION .. "!")
            return "Hello, world!", 42, true
            LUA,
    ],
    'json-demo' => [
        'label' => 'json library + input',
        'description' => 'The PHP json bridge, fed from the input panel.',
        'requiredCapabilities' => [],
        'seedFiles' => [],
        'lua' => <<<'LUA'
            -- `input` is the decoded JSON from the input panel (also the chunk's
            -- first vararg). Note: a JSON array arrives 0-indexed from PHP, so its
            -- first element sits outside Lua's `#` sequence.
            local payload = input or { name = "Ada", languages = { "Lua", "PHP" } }
            local encoded = json.encode(payload)
            print("Encoded: " .. encoded)
            local decoded = json.decode(encoded)
            print("Round-tripped name: " .. tostring(decoded.name))
            return encoded, decoded.name
            LUA,
    ],
    'html-demo' => [
        'label' => 'html library (DOMDocument)',
        'description' => 'PHP DOMDocument parsing exposed to Lua.',
        'requiredCapabilities' => [],
        'seedFiles' => [],
        'lua' => <<<'LUA'
            local page = [[
            <html><head><title>Playground</title></head>
            <body><p>Hello there.</p><a href="https://example.com">Example</a>
            <a href="/docs">Docs</a></body></html>
            ]]
            print("Title: " .. tostring(html.title(page)))
            print("Text: " .. html.text(page))
            local links = html.links(page)
            for index = 1, #links do
                print(index .. ": " .. links[index])
            end
            return html.title(page), links
            LUA,
    ],
    'toolkit-demo' => [
        'label' => 'toolkit object (#[LuaMethod])',
        'description' => 'registerObject: one PHP instance backing a whole run.',
        'requiredCapabilities' => [],
        'seedFiles' => [],
        'lua' => <<<'LUA'
            toolkit.reset()
            toolkit.remember("greeting", "hello")
            for step = 1, 5 do
                toolkit.increment(step)
            end
            print("Counter: " .. toolkit.value())
            print("Remembered: " .. tostring(toolkit.recall("greeting")))
            return toolkit.value(), toolkit.recall("greeting")
            LUA,
    ],
    'host-classes-demo' => [
        'label' => 'User-defined host classes',
        'description' => 'Uses the default greeter/mathx entries from the Host classes panel.',
        'requiredCapabilities' => [],
        'seedFiles' => [],
        'lua' => <<<'LUA'
            -- These names come from the Host classes panel below the VFS.
            print(greeter.hello("World"))
            print("7 squared is " .. mathx.square(7))
            print("Is 7 even? " .. tostring(mathx.isEven(7)))
            return mathx.square(7), mathx.isEven(7)
            LUA,
    ],
    'vfs-demo' => [
        'label' => 'VFS read + write',
        'description' => 'io.open against the session-persisted in-memory filesystem.',
        'requiredCapabilities' => ['vfs', 'vfsWrite'],
        'seedFiles' => ['/greeting.txt' => "Hello from the VFS!\n"],
        'lua' => <<<'LUA'
            local file, openError = io.open("/greeting.txt", "r")
            if not file then
                error("could not open /greeting.txt: " .. tostring(openError))
            end
            local original = file:read("a")
            file:close()
            print("Read: " .. original)

            local writer = assert(io.open("/greeting.txt", "a"))
            writer:write("(visited by Lua)\n")
            writer:close()
            -- The write persists: run this again, or open the file in the VFS panel.
            return original
            LUA,
    ],
    'binary-file-demo' => [
        'label' => 'Binary file (hex viewer)',
        'description' => 'Writes raw bytes; view /data.bin in the VFS panel as an xxd dump.',
        'requiredCapabilities' => ['vfs', 'vfsWrite'],
        'seedFiles' => [],
        'lua' => <<<'LUA'
            local file = assert(io.open("/data.bin", "wb"))
            for byte = 0, 255 do
                file:write(string.char(byte))
            end
            file:close()

            local reader = assert(io.open("/data.bin", "rb"))
            local readBack = reader:read("a")
            reader:close()
            -- Now open /data.bin with the View button in the VFS panel.
            return #readBack
            LUA,
    ],
    'require-demo' => [
        'label' => 'require() from the VFS',
        'description' => 'Module resolution along modulePaths against the VFS.',
        'requiredCapabilities' => ['require', 'vfs'],
        'seedFiles' => [
            '/greetlib.lua' => "local M = {}\nfunction M.shout(text)\n    return string.upper(text) .. \"!\"\nend\nreturn M\n",
        ],
        'lua' => <<<'LUA'
            local greetlib = require("greetlib")
            print(greetlib.shout("hello from require"))
            return greetlib.shout("hello")
            LUA,
    ],
    'cpu-limit-demo' => [
        'label' => 'CPU limit (uncatchable)',
        'description' => 'An infinite loop stopped by CpuLimitError.',
        'requiredCapabilities' => [],
        'seedFiles' => [],
        'lua' => <<<'LUA'
            -- Runs forever; the CPU limit stops it with CpuLimitError. Try wrapping
            -- the loop in pcall — it still stops: fatal errors are re-raised
            -- through pcall/xpcall by design.
            local total = 0
            while true do
                total = total + 1
            end
            return total
            LUA,
    ],
    'memory-limit-demo' => [
        'label' => 'Memory limit',
        'description' => 'Grows a table until MemoryLimitError trips.',
        'requiredCapabilities' => [],
        'seedFiles' => [],
        'lua' => <<<'LUA'
            local hoard = {}
            local index = 0
            while true do
                index = index + 1
                hoard[index] = string.rep("x", 1024)
            end
            return index
            LUA,
    ],
    'coroutines-demo' => [
        'label' => 'Coroutines',
        'description' => 'coroutine.wrap and the coroutine stats fields.',
        'requiredCapabilities' => [],
        'seedFiles' => [],
        'lua' => <<<'LUA'
            local function fibonacci(count)
                return coroutine.wrap(function()
                    local previous, current = 0, 1
                    for _ = 1, count do
                        coroutine.yield(current)
                        previous, current = current, previous + current
                    end
                end)
            end

            local results = {}
            for value in fibonacci(10) do
                results[#results + 1] = value
            end
            return table.unpack(results)
            LUA,
    ],
    'profiler-demo' => [
        'label' => 'Profiler',
        'description' => 'Enable the profiler toggle, then run this.',
        'requiredCapabilities' => [],
        'seedFiles' => [],
        'lua' => <<<'LUA'
            -- Enable the profiler in the configuration panel before running.
            local function hot(iterations)
                local total = 0
                for index = 1, iterations do
                    total = total + index * 0.5
                end
                return total
            end
            local function cold(iterations)
                local total = 0
                for index = 1, iterations do
                    total = total + 1
                end
                return total
            end
            for round = 1, 20 do
                hot(200000)
                cold(10000)
            end
            return "done"
            LUA,
    ],
];

/**
 * Serve the UI. All dynamic data rides in one JSON bootstrap blob; the markup
 * itself is static (a NOWDOC, so the inline JS can use $ freely).
 */
function renderPlaygroundPage(PlaygroundFileSystem $filesystem): void
{
    $bootstrap = [
        'extensionVersion' => Sandbox::extensionVersion(),
        'luaVersion' => Sandbox::luaVersion(),
        'features' => presentFeaturesForDisplay(),
        'xdebugLoaded' => extension_loaded('xdebug'),
        'vfsState' => $filesystem->snapshotListing(),
        'presets' => PRESET_EXAMPLES,
        'chunkName' => ltrim(CHUNK_NAME, '@='),
        'runSecondsCap' => MAX_RUN_SECONDS,
    ];

    $bootstrapJson = json_encode(
        $bootstrap,
        JSON_HEX_TAG | JSON_HEX_AMP | JSON_UNESCAPED_SLASHES | JSON_INVALID_UTF8_SUBSTITUTE,
    );

    header('Content-Type: text/html; charset=utf-8');
    echo str_replace('__PLAYGROUND_BOOTSTRAP_JSON__', $bootstrapJson === false ? '{}' : $bootstrapJson, playgroundPageTemplate());
}

/**
 * The full static page. Kept as one NOWDOC so nothing in the CSS/JS is subject
 * to PHP interpolation.
 */
function playgroundPageTemplate(): string
{
    return <<<'PLAYGROUND_HTML'
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>LuaExt Playground</title>
<style>
:root {
    color-scheme: light dark;
    --paper: #f6f6f9;
    --panel: #ffffff;
    --ink: #26282e;
    --muted: #6d7280;
    --line: #d9dbe4;
    --line-soft: #e6e8ef;
    --accent: #383d8f;      /* Lua's moon-navy */
    --accent-soft: #383d8f22;
    --on-accent: #ffffff;
    --code-bg: #f0f1f6;
    --ok: #2e7d43;
    --err: #b3383e;
    --warn-bg: #f5c84222;
    --warn-line: #b8860b66;
    --danger-bg: #c8404022;
    --danger-line: #c8404066;
    --mono: ui-monospace, SFMono-Regular, Menlo, Consolas, monospace;
}
@media (prefers-color-scheme: dark) {
    :root {
        --paper: #14161b;
        --panel: #1c1f26;
        --ink: #d9dce3;
        --muted: #8d92a2;
        --line: #363a47;
        --line-soft: #2b2e39;
        --accent: #9aa0e8;
        --accent-soft: #9aa0e822;
        --on-accent: #14162e;
        --code-bg: #12141a;
        --ok: #74c28a;
        --err: #e2898e;
        --warn-bg: #f5c8421c;
        --danger-bg: #c840401f;
    }
}
* { box-sizing: border-box; }
body {
    font-family: system-ui, sans-serif;
    background: var(--paper);
    color: var(--ink);
    margin: 0 auto;
    max-width: 1250px;
    padding: 0 1.25rem 3rem;
    line-height: 1.5;
}
h1 {
    font-size: 1.35rem;
    font-weight: 650;
    letter-spacing: -0.01em;
    margin: 1.25rem 0 0.15rem;
}
h2 { font-size: 1rem; font-weight: 650; margin: 0 0 0.6rem; }
h3 { font-size: 0.9rem; font-weight: 650; margin: 0.9rem 0 0.3rem; }
code { font-family: var(--mono); font-size: 0.85em; background: var(--code-bg); padding: 0.05em 0.3em; border-radius: 4px; }
section {
    background: var(--panel);
    border: 1px solid var(--line);
    border-radius: 8px;
    padding: 0.9rem 1.1rem 1rem;
    margin: 0.9rem 0;
}
textarea, input, select, button { font: inherit; color: inherit; }
textarea, input[type="text"], input[type="number"], select {
    background: var(--panel);
    border: 1px solid var(--line);
    border-radius: 5px;
    padding: 0.2rem 0.45rem;
}
textarea { width: 100%; font-family: var(--mono); font-size: 0.85rem; line-height: 1.45; padding: 0.5rem 0.6rem; }
textarea:focus-visible, input:focus-visible, select:focus-visible, button:focus-visible {
    outline: 2px solid var(--accent);
    outline-offset: 1px;
}
pre {
    background: var(--code-bg);
    border: 1px solid var(--line-soft);
    padding: 0.5rem 0.7rem;
    border-radius: 6px;
    overflow-x: auto;
    font-family: var(--mono);
    font-size: 0.8rem;
    line-height: 1.5;
}
table { border-collapse: collapse; width: 100%; }
td, th {
    border: 1px solid var(--line-soft);
    padding: 0.25rem 0.6rem;
    font-size: 0.85rem;
    text-align: left;
    vertical-align: top;
}
th { font-weight: 550; }
#stats-table-body td, #profile-table-body td { font-family: var(--mono); font-size: 0.8rem; text-align: right; }
button {
    cursor: pointer;
    background: var(--panel);
    border: 1px solid var(--line);
    border-radius: 5px;
    padding: 0.28rem 0.8rem;
}
button:hover { border-color: var(--accent); }
button:disabled { cursor: default; opacity: 0.55; border-color: var(--line); }
button.primary {
    background: var(--accent);
    border-color: var(--accent);
    color: var(--on-accent);
    font-weight: 600;
    padding-inline: 1.1rem;
}
button.primary:hover:not(:disabled) { filter: brightness(1.1); }
.banner { padding: 0.55rem 0.8rem; border-radius: 6px; margin: 0.5rem 0; font-size: 0.9rem; }
.banner.warning { background: var(--warn-bg); border: 1px solid var(--warn-line); }
.banner.danger { background: var(--danger-bg); border: 1px solid var(--danger-line); }
.grid { display: grid; grid-template-columns: repeat(auto-fill, minmax(220px, 1fr)); gap: 0.15rem 1.25rem; }
.field { display: flex; align-items: center; justify-content: space-between; gap: 0.6rem; padding: 0.12rem 0; }
.field label { flex: 1; }
.field input[type="number"], .field input[type="text"] { width: 10rem; font-family: var(--mono); font-size: 0.85rem; }
.checkbox-field { display: flex; align-items: center; gap: 0.45rem; padding: 0.12rem 0; }
.checkbox-field input, .field input[type="checkbox"] { accent-color: var(--accent); }
.editor-grid { display: grid; grid-template-columns: minmax(0, 3fr) minmax(0, 2fr); gap: 0 1rem; }
.config-grid { display: grid; grid-template-columns: repeat(2, minmax(0, 1fr)); gap: 0 1rem; }
@media (max-width: 950px) { .editor-grid, .config-grid { grid-template-columns: 1fr; } }
.muted { color: var(--muted); font-size: 0.85rem; }
select { padding-block: 0.22rem; }
.status-ok { color: var(--ok); font-weight: 600; }
.status-error { color: var(--err); font-weight: 600; }
.error-inline { color: var(--err); font-size: 0.85rem; white-space: pre-wrap; }
.lua-nil { color: var(--muted); font-style: italic; }
.lua-string { color: var(--ok); }
.lua-scalar { color: var(--accent); }
.lua-function { color: var(--muted); font-style: italic; }
.lua-string, .lua-scalar { font-family: var(--mono); font-size: 0.85rem; }
.value-table { margin: 0.15rem 0; width: auto; }
.value-table td { font-family: var(--mono); font-size: 0.8rem; }
.return-value { margin: 0.35rem 0; display: flex; gap: 0.6rem; align-items: baseline; }
.return-index { color: var(--muted); font-family: var(--mono); font-size: 0.8rem; }
.host-class-entry { border: 1px solid var(--line); border-radius: 6px; padding: 0.6rem 0.75rem; margin: 0.55rem 0; background: var(--paper); }
.host-class-entry .name-row { display: flex; gap: 0.5rem; align-items: center; margin-bottom: 0.4rem; }
.host-class-entry .name-row input { font-family: var(--mono); font-size: 0.85rem; flex: 1; }
.row-actions { white-space: nowrap; }
.row-actions button { margin-right: 0.3rem; font-size: 0.85rem; padding: 0.15rem 0.55rem; }
.badge {
    display: inline-block;
    border: 1px solid var(--line);
    border-radius: 4px;
    padding: 0 0.4rem;
    font-family: var(--mono);
    font-size: 0.75rem;
    color: var(--muted);
}
.badge.stderr { border-color: var(--err); color: var(--err); }
#error-category { border-color: var(--err); color: var(--err); }
dialog {
    max-width: min(90vw, 900px);
    background: var(--panel);
    color: var(--ink);
    border: 1px solid var(--line);
    border-radius: 8px;
    padding: 1rem 1.2rem;
}
dialog::backdrop { background: #00000059; }
dialog pre { max-height: 65vh; overflow: auto; }
#file-viewer-title { font-family: var(--mono); margin-top: 0; }
details { margin: 0.4rem 0; }
details > summary { cursor: pointer; font-size: 0.9rem; color: var(--muted); }
details > summary:hover { color: var(--ink); }
details[open] > summary { margin-bottom: 0.35rem; }
.actions { display: flex; gap: 0.5rem; margin: 0.6rem 0 0.4rem; flex-wrap: wrap; align-items: center; }
#vfs-table td:first-child { font-family: var(--mono); font-size: 0.8rem; }
#header-meta { font-family: var(--mono); font-size: 0.78rem; color: var(--muted); margin: 0 0 0.75rem; }
h2 .muted { font-weight: 400; }
.hidden { display: none !important; }
</style>
</head>
<body>

<h1>LuaExt Playground</h1>
<p id="header-meta"></p>
<div id="xdebug-banner" class="banner warning hidden">
Xdebug is loaded in this PHP — CPU/wall-clock timings, limits, and profiler numbers will read high. Disable it for representative measurements.
</div>
<div class="banner danger">
Loopback-only development tool. The <strong>Host classes</strong> panel evaluates real PHP with this process's full privileges — only the <em>Lua</em> side of the bridge is sandboxed.
</div>

<div class="editor-grid">
<div>

<section>
<h2>Script</h2>
<div class="actions">
    <label for="preset-select">Preset</label>
    <select id="preset-select"></select>
    <span id="preset-description" class="muted"></span>
</div>
<textarea id="source-editor" rows="16" spellcheck="false"></textarea>
<p class="muted">Runs as chunk <code id="chunk-name-label"></code>.</p>
<h3>Input (JSON)</h3>
<textarea id="input-editor" rows="5" spellcheck="false"></textarea>
<p class="muted">Decoded and handed to the script as the global <code>input</code> and as the chunk's vararg (<code>local input = ...</code>). JSON arrays arrive 0-indexed — key 0 sits outside Lua's <code>#</code> sequence.</p>
<div class="actions">
    <button id="run-button" class="primary">Run</button>
    <button id="validate-button">Validate</button>
    <span id="run-status"></span>
</div>
</section>

</div>
<div>

<section>
<h2>Results</h2>
<p id="results-empty-note" class="muted">No run yet.</p>
<div id="error-panel" class="hidden">
    <p><span id="error-category" class="badge"></span> <strong id="error-class"></strong></p>
    <p id="error-message" class="error-inline"></p>
    <pre id="error-trace" class="hidden"></pre>
</div>
<div id="validation-panel" class="hidden"><p id="validation-message"></p></div>
<div id="values-panel" class="hidden"><h3>Return values</h3><div id="values-list"></div></div>
<div id="output-panel" class="hidden"><h3>Output</h3><pre id="output-content"></pre><p id="output-truncated-note" class="muted hidden">Output was truncated (outputBytes limit, Truncate mode).</p></div>
<div id="chunks-panel" class="hidden"><h3>Callback chunks</h3><div id="chunks-list"></div></div>
<div id="warnings-panel" class="hidden"><h3>Warnings</h3><ul id="warnings-list"></ul></div>
</section>

<section>
<h2>Stats</h2>
<p id="stats-empty-note" class="muted">Run a script to populate the stats.</p>
<div id="stats-panel" class="hidden">
    <table><tbody id="stats-table-body"></tbody></table>
    <details><summary>Raw stats JSON</summary><pre id="stats-raw"></pre></details>
</div>
<div id="profile-panel" class="hidden"><h3>Profile (<span id="profile-unit"></span>)</h3><table><tbody id="profile-table-body"></tbody></table></div>
</section>

</div>
</div>

<div class="config-grid">
<div>

<section>
<h2>Capabilities</h2>
<div class="actions">
    <button id="capabilities-untrusted">Untrusted preset</button>
    <button id="capabilities-trusted">Trusted preset</button>
</div>
<div id="capability-grid" class="grid"></div>
<div class="field"><label for="capability-osEnvAllowList">osEnvAllowList <span class="muted">(comma-separated)</span></label><input type="text" id="capability-osEnvAllowList"></div>
<p class="muted">vfs/vfsWrite use the session VFS below. debugHooks always fails here: it requires unlimited CPU + wall-clock, which this playground never grants.</p>
</section>

</div>
<div>

<section>
<h2>Limits</h2>
<div id="limit-grid"></div>
<details><summary>Advanced limits</summary><div id="limit-advanced-grid"></div></details>
<div class="field"><label for="limit-outputOverflow">outputOverflow</label>
<select id="limit-outputOverflow"><option>Fail</option><option>Truncate</option></select></div>
<p class="muted">cpuSeconds and wallClockSeconds are capped at <span id="seconds-cap"></span>s by the playground (php -S is single-worker).</p>
<details><summary>VFS quota</summary><div id="quota-grid"></div>
<div class="checkbox-field"><input type="checkbox" id="quota-billWallTime"><label for="quota-billWallTime">billWallTime</label></div>
</details>
</section>

<section>
<h2>Execution options</h2>
<div class="field"><label for="output-mode">Output mode</label>
<select id="output-mode"><option>Buffer</option><option>Callback</option><option>Discard</option></select></div>
<div class="field"><label for="output-chunk-bytes">outputChunkBytes</label><input type="number" id="output-chunk-bytes" min="1"></div>
<div class="checkbox-field"><input type="checkbox" id="profiler-enabled"><label for="profiler-enabled">Enable sampling profiler</label></div>
<div class="field"><label for="profiler-period">Profiler period (s)</label><input type="number" id="profiler-period" step="0.001" min="0.0001"></div>
<div class="field"><label for="profiler-unit">Profiler unit</label>
<select id="profiler-unit"><option>Seconds</option><option>Samples</option><option>Percent</option></select></div>
<div class="checkbox-field"><input type="checkbox" id="deterministic"><label for="deterministic">deterministic</label></div>
<div class="field"><label for="seed">seed</label><input type="number" id="seed" disabled></div>
<div class="checkbox-field"><input type="checkbox" id="cache-compiled-chunks"><label for="cache-compiled-chunks">cacheCompiledChunks <span class="muted">(no-op here: fresh sandbox per run)</span></label></div>
</section>

</div>
</div>

<section>
<h2>VFS <span class="muted">(in-memory, persists in your session)</span></h2>
<table id="vfs-table"><thead><tr><th>Path</th><th>Size</th><th>Modified</th><th></th></tr></thead><tbody id="vfs-table-body"></tbody></table>
<p id="vfs-empty-note" class="muted">No files yet.</p>
<h3>Add / edit file</h3>
<div class="field"><label for="vfs-path">Path</label><input type="text" id="vfs-path" placeholder="/example.txt"></div>
<textarea id="vfs-content" rows="5" spellcheck="false"></textarea>
<div class="actions">
    <button id="vfs-save-button">Save file</button>
    <button id="vfs-reset-button">Reset VFS</button>
    <span id="vfs-status" class="muted"></span>
</div>
</section>

<section>
<h2>Host classes <span class="muted">(unsandboxed PHP)</span></h2>
<p class="muted">Each snippet is <code>eval()</code>'d per run and must <code>return</code> an object (registered via <code>registerObject</code>; <code>#[LuaMethod]</code>-marked methods become visible) or an <code>array&lt;string, callable&gt;</code> (registered via <code>registerLibrary</code>). Stored in your browser, sent with every run.</p>
<div id="host-class-list"></div>
<div class="actions"><button id="host-class-add">Add host class</button></div>
</section>

<dialog id="file-viewer">
<h3 id="file-viewer-title"></h3>
<p id="file-viewer-meta" class="muted"></p>
<pre id="file-viewer-content"></pre>
<form method="dialog"><button>Close</button></form>
</dialog>

<script type="application/json" id="playground-bootstrap">__PLAYGROUND_BOOTSTRAP_JSON__</script>
<script>
'use strict';

const bootstrap = JSON.parse(document.getElementById('playground-bootstrap').textContent);
const STORAGE_KEY = 'luaextPlaygroundState';
const byId = (elementId) => document.getElementById(elementId);

// Capability flags with their untrusted defaults and the trusted() preset values.
const capabilityDefinitions = [
    { key: 'loadBytecode', defaultValue: false, trustedValue: false },
    { key: 'compileAtRuntime', defaultValue: false, trustedValue: true },
    { key: 'dumpBytecode', defaultValue: false, trustedValue: true },
    { key: 'require', defaultValue: false, trustedValue: true },
    { key: 'vfs', defaultValue: false, trustedValue: true },
    { key: 'vfsWrite', defaultValue: false, trustedValue: false },
    { key: 'coroutines', defaultValue: true, trustedValue: true },
    { key: 'osTime', defaultValue: true, trustedValue: true },
    { key: 'osEnv', defaultValue: false, trustedValue: false },
    { key: 'debugTraceback', defaultValue: true, trustedValue: true },
    { key: 'debugIntrospect', defaultValue: false, trustedValue: true },
    { key: 'debugMutate', defaultValue: false, trustedValue: false },
    { key: 'debugHooks', defaultValue: false, trustedValue: false },
    { key: 'utf8', defaultValue: true, trustedValue: true },
    { key: 'gcControl', defaultValue: false, trustedValue: true },
    { key: 'warn', defaultValue: false, trustedValue: true },
];

const limitDefinitions = [
    { key: 'memoryBytes', defaultValue: 33554432, advanced: false },
    { key: 'cpuSeconds', defaultValue: 1, advanced: false, isFloat: true },
    { key: 'wallClockSeconds', defaultValue: 5, advanced: false, isFloat: true },
    { key: 'outputBytes', defaultValue: 1048576, advanced: false },
    { key: 'maxLiveCoroutines', defaultValue: 64, advanced: true },
    { key: 'maxCoroutineDepth', defaultValue: 16, advanced: true },
    { key: 'maxCallDepth', defaultValue: 200, advanced: true },
    { key: 'maxModules', defaultValue: 64, advanced: true },
    { key: 'maxRequireDepth', defaultValue: 16, advanced: true },
    { key: 'maxStringLength', defaultValue: 67108864, advanced: true },
    { key: 'maxSourceBytes', defaultValue: 1048576, advanced: true },
    { key: 'maxConversionDepth', defaultValue: 64, advanced: true },
    { key: 'maxCachedChunks', defaultValue: 64, advanced: true },
];

const quotaDefinitions = [
    { key: 'maxOpenHandles', defaultValue: 16 },
    { key: 'maxFileBytes', defaultValue: 1048576 },
    { key: 'maxTotalBytes', defaultValue: 8388608 },
    { key: 'maxFiles', defaultValue: 128 },
    { key: 'maxOperations', defaultValue: 10000 },
    { key: 'maxPathLength', defaultValue: 255 },
    { key: 'maxPathDepth', defaultValue: 16 },
];

const defaultHostClasses = [
    {
        luaName: 'greeter',
        phpSource: [
            'return new class {',
            '    #[\\DevelopGravity\\LuaExt\\LuaMethod(\'hello\')]',
            '    public function sayHello(string $name): string',
            '    {',
            '        return "Hello, {$name}! (from a user-defined PHP class)";',
            '    }',
            '};',
        ].join('\n'),
    },
    {
        luaName: 'mathx',
        phpSource: [
            'return [',
            "    'square' => static fn (int $number): int => $number * $number,",
            "    'isEven' => static fn (int $number): bool => $number % 2 === 0,",
            '];',
        ].join('\n'),
    },
];

const statFieldOrder = [
    'memoryBytes', 'peakMemoryBytes', 'memoryLimitBytes', 'cpuSeconds', 'wallClockSeconds',
    'outputBytes', 'outputTruncated', 'liveCoroutines', 'peakCoroutineDepth', 'modulesLoaded',
    'cachedChunks', 'vfsOperations', 'vfsBytes', 'gcCollections', 'luaCallsIn', 'phpCallsOut',
];

let hostClasses = [];
let lastVfsListing = bootstrap.vfsState || [];

function defaultState() {
    return {
        source: bootstrap.presets.hello.lua,
        inputJson: JSON.stringify({ name: 'Ada', languages: ['Lua', 'PHP'] }, null, 2),
        capabilities: Object.fromEntries(capabilityDefinitions.map((definition) => [definition.key, definition.defaultValue])),
        osEnvAllowList: '',
        limits: Object.fromEntries(limitDefinitions.map((definition) => [definition.key, definition.defaultValue])),
        outputOverflow: 'Fail',
        vfsQuota: Object.fromEntries(quotaDefinitions.map((definition) => [definition.key, definition.defaultValue])),
        billWallTime: false,
        outputMode: 'Buffer',
        outputChunkBytes: 8192,
        profilerEnabled: false,
        profilerPeriodSeconds: 0.002,
        profilerUnit: 'Seconds',
        deterministic: false,
        seed: '',
        cacheCompiledChunks: false,
        hostClasses: defaultHostClasses,
        presetId: 'hello',
    };
}

function element(tagName, className, textContent) {
    const node = document.createElement(tagName);
    if (className) node.className = className;
    if (textContent !== undefined) node.textContent = textContent;
    return node;
}

function clearChildren(node) {
    while (node.firstChild) node.removeChild(node.firstChild);
}

function buildConfigInputs() {
    const capabilityGrid = byId('capability-grid');
    for (const definition of capabilityDefinitions) {
        const wrapper = element('div', 'checkbox-field');
        const checkbox = document.createElement('input');
        checkbox.type = 'checkbox';
        checkbox.id = 'capability-' + definition.key;
        const label = document.createElement('label');
        label.htmlFor = checkbox.id;
        label.textContent = definition.key;
        wrapper.append(checkbox, label);
        capabilityGrid.append(wrapper);
    }

    for (const definition of limitDefinitions) {
        const wrapper = element('div', 'field');
        const label = document.createElement('label');
        label.htmlFor = 'limit-' + definition.key;
        label.textContent = definition.key;
        const numberInput = document.createElement('input');
        numberInput.type = 'number';
        numberInput.id = 'limit-' + definition.key;
        if (definition.isFloat) numberInput.step = '0.1';
        wrapper.append(label, numberInput);
        byId(definition.advanced ? 'limit-advanced-grid' : 'limit-grid').append(wrapper);
    }

    for (const definition of quotaDefinitions) {
        const wrapper = element('div', 'field');
        const label = document.createElement('label');
        label.htmlFor = 'quota-' + definition.key;
        label.textContent = definition.key;
        const numberInput = document.createElement('input');
        numberInput.type = 'number';
        numberInput.id = 'quota-' + definition.key;
        wrapper.append(label, numberInput);
        byId('quota-grid').append(wrapper);
    }
}

function applyStateToForm(state) {
    byId('source-editor').value = state.source;
    byId('input-editor').value = state.inputJson;
    for (const definition of capabilityDefinitions) {
        byId('capability-' + definition.key).checked = Boolean(state.capabilities[definition.key]);
    }
    byId('capability-osEnvAllowList').value = state.osEnvAllowList;
    for (const definition of limitDefinitions) {
        byId('limit-' + definition.key).value = state.limits[definition.key];
    }
    byId('limit-outputOverflow').value = state.outputOverflow;
    for (const definition of quotaDefinitions) {
        byId('quota-' + definition.key).value = state.vfsQuota[definition.key];
    }
    byId('quota-billWallTime').checked = state.billWallTime;
    byId('output-mode').value = state.outputMode;
    byId('output-chunk-bytes').value = state.outputChunkBytes;
    byId('profiler-enabled').checked = state.profilerEnabled;
    byId('profiler-period').value = state.profilerPeriodSeconds;
    byId('profiler-unit').value = state.profilerUnit;
    byId('deterministic').checked = state.deterministic;
    byId('seed').value = state.seed;
    byId('seed').disabled = !state.deterministic;
    byId('cache-compiled-chunks').checked = state.cacheCompiledChunks;
    byId('preset-select').value = state.presetId;
    hostClasses = Array.isArray(state.hostClasses) ? state.hostClasses : [];
    renderHostClassEditor();
    updatePresetDescription();
}

function numberOrNull(rawValue, isFloat) {
    const parsed = isFloat ? parseFloat(rawValue) : parseInt(rawValue, 10);
    return Number.isFinite(parsed) ? parsed : null;
}

function collectStateFromForm() {
    return {
        source: byId('source-editor').value,
        inputJson: byId('input-editor').value,
        capabilities: Object.fromEntries(capabilityDefinitions.map(
            (definition) => [definition.key, byId('capability-' + definition.key).checked],
        )),
        osEnvAllowList: byId('capability-osEnvAllowList').value,
        limits: Object.fromEntries(limitDefinitions.map(
            (definition) => [definition.key, numberOrNull(byId('limit-' + definition.key).value, definition.isFloat)],
        )),
        outputOverflow: byId('limit-outputOverflow').value,
        vfsQuota: Object.fromEntries(quotaDefinitions.map(
            (definition) => [definition.key, numberOrNull(byId('quota-' + definition.key).value, false)],
        )),
        billWallTime: byId('quota-billWallTime').checked,
        outputMode: byId('output-mode').value,
        outputChunkBytes: numberOrNull(byId('output-chunk-bytes').value, false),
        profilerEnabled: byId('profiler-enabled').checked,
        profilerPeriodSeconds: numberOrNull(byId('profiler-period').value, true),
        profilerUnit: byId('profiler-unit').value,
        deterministic: byId('deterministic').checked,
        seed: byId('seed').value,
        cacheCompiledChunks: byId('cache-compiled-chunks').checked,
        hostClasses: hostClasses,
        presetId: byId('preset-select').value,
    };
}

function persistState() {
    try {
        localStorage.setItem(STORAGE_KEY, JSON.stringify(collectStateFromForm()));
    } catch (storageError) {
        // localStorage disabled/full: state simply will not survive a reload.
    }
}

function loadState() {
    const fallback = defaultState();
    try {
        const stored = JSON.parse(localStorage.getItem(STORAGE_KEY) || 'null');
        if (!stored || typeof stored !== 'object') return fallback;
        return {
            ...fallback,
            ...stored,
            capabilities: { ...fallback.capabilities, ...(stored.capabilities || {}) },
            limits: { ...fallback.limits, ...(stored.limits || {}) },
            vfsQuota: { ...fallback.vfsQuota, ...(stored.vfsQuota || {}) },
        };
    } catch (storageError) {
        return fallback;
    }
}

function collectPayload() {
    const state = collectStateFromForm();
    return {
        source: state.source,
        inputJson: state.inputJson,
        capabilities: { ...state.capabilities, osEnvAllowList: state.osEnvAllowList },
        limits: { ...state.limits, outputOverflow: state.outputOverflow },
        vfsQuota: { ...state.vfsQuota, billWallTime: state.billWallTime },
        outputMode: state.outputMode,
        outputChunkBytes: state.outputChunkBytes,
        profilerEnabled: state.profilerEnabled,
        profilerPeriodSeconds: state.profilerPeriodSeconds,
        profilerUnit: state.profilerUnit,
        deterministic: state.deterministic,
        seed: state.seed,
        cacheCompiledChunks: state.cacheCompiledChunks,
        hostClasses: state.hostClasses.filter((entry) => entry.luaName || entry.phpSource),
    };
}

async function callPlaygroundAction(action, extraFields = {}) {
    const payload = Object.assign(collectPayload(), { action }, extraFields);
    const response = await fetch(location.pathname, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(payload),
    });
    const data = await response.json();
    if (Array.isArray(data.vfsState)) {
        lastVfsListing = data.vfsState;
        renderVfsListing(data.vfsState);
    }
    return data;
}

// ---- Rendering -------------------------------------------------------------

function renderValue(value) {
    if (value === null) return element('span', 'lua-nil', 'nil');
    if (typeof value === 'boolean' || typeof value === 'number') return element('span', 'lua-scalar', String(value));
    if (typeof value === 'string') return element('span', 'lua-string', JSON.stringify(value));
    if (typeof value !== 'object') return element('span', 'lua-scalar', String(value));

    switch (value.__kind) {
        case 'table': {
            const table = element('table', 'value-table');
            const tableBody = document.createElement('tbody');
            for (const entry of value.entries) {
                const row = document.createElement('tr');
                if (entry.keyType === 'truncated') {
                    const cell = document.createElement('td');
                    cell.colSpan = 2;
                    cell.className = 'lua-nil';
                    cell.textContent = entry.value;
                    row.append(cell);
                    tableBody.append(row);
                    continue;
                }
                const keyCell = document.createElement('td');
                keyCell.textContent = entry.keyType === 'string' ? JSON.stringify(entry.key) : String(entry.key);
                const valueCell = document.createElement('td');
                valueCell.append(renderValue(entry.value));
                row.append(keyCell, valueCell);
                tableBody.append(row);
            }
            table.append(tableBody);
            return table;
        }
        case 'binaryString': {
            const details = document.createElement('details');
            details.append(element('summary', 'lua-function', 'binary string (' + value.length + ' bytes)'));
            details.append(element('pre', null, 'base64: ' + value.base64));
            return details;
        }
        case 'luaFunction':
            return element('span', 'lua-function', value.valid ? 'Lua function' : 'Lua function (invalidated)');
        case 'nonFiniteFloat':
            return element('span', 'lua-scalar', value.value);
        case 'unrepresentable':
            return element('span', 'lua-nil', 'unrepresentable PHP value: ' + value.phpType);
        default:
            return element('span', 'lua-scalar', JSON.stringify(value));
    }
}

function formatStatValue(fieldName, value) {
    if (typeof value === 'boolean') return value ? 'yes' : 'no';
    if (fieldName === 'memoryLimitBytes' && value === 0) return 'unlimited';
    if (fieldName.endsWith('Seconds')) return (value * 1000).toFixed(3) + ' ms';
    if (fieldName.endsWith('Bytes')) return value.toLocaleString() + ' B';
    return value.toLocaleString();
}

function renderStats(stats) {
    const tableBody = byId('stats-table-body');
    clearChildren(tableBody);
    for (const fieldName of statFieldOrder) {
        if (!(fieldName in stats)) continue;
        const row = document.createElement('tr');
        row.append(element('th', null, fieldName), element('td', null, formatStatValue(fieldName, stats[fieldName])));
        tableBody.append(row);
    }
    byId('stats-raw').textContent = JSON.stringify(stats, null, 2);
    byId('stats-panel').classList.remove('hidden');
    byId('stats-empty-note').classList.add('hidden');
}

function renderProfile(profile) {
    const panel = byId('profile-panel');
    if (!profile || !profile.entries || Object.keys(profile.entries).length === 0) {
        panel.classList.add('hidden');
        return;
    }
    byId('profile-unit').textContent = profile.unit;
    const tableBody = byId('profile-table-body');
    clearChildren(tableBody);
    const sortedEntries = Object.entries(profile.entries).sort((first, second) => second[1] - first[1]);
    for (const [functionName, sampleValue] of sortedEntries) {
        const row = document.createElement('tr');
        row.append(element('th', null, functionName), element('td', null, String(sampleValue)));
        tableBody.append(row);
    }
    panel.classList.remove('hidden');
}

function hideAllResultPanels() {
    for (const panelId of ['error-panel', 'validation-panel', 'values-panel', 'output-panel',
        'chunks-panel', 'warnings-panel', 'stats-panel', 'profile-panel']) {
        byId(panelId).classList.add('hidden');
    }
    byId('stats-empty-note').classList.remove('hidden');
    byId('results-empty-note').classList.add('hidden');
}

function renderErrorPanel(data) {
    byId('error-category').textContent = data.errorCategory || 'Error';
    byId('error-class').textContent = data.errorClass || '';
    byId('error-message').textContent = data.message || '';
    const tracePre = byId('error-trace');
    const traceText = data.luaTraceFormatted || data.trace || '';
    tracePre.textContent = traceText;
    tracePre.classList.toggle('hidden', traceText === '');
    byId('error-panel').classList.remove('hidden');
}

function renderWarnings(data) {
    const warnings = [...(data.warnings || [])];
    for (const hostClassError of data.hostClassErrors || []) {
        warnings.push('host class ' + (hostClassError.luaName || '(unnamed)') + ': ' + hostClassError.message);
    }
    if (warnings.length === 0) return;
    const warningsList = byId('warnings-list');
    clearChildren(warningsList);
    for (const warning of warnings) warningsList.append(element('li', null, warning));
    byId('warnings-panel').classList.remove('hidden');
}

function renderHostClassErrorsInline(hostClassErrors) {
    document.querySelectorAll('.host-class-error').forEach((node) => { node.textContent = ''; });
    for (const hostClassError of hostClassErrors || []) {
        const index = hostClasses.findIndex((entry) => entry.luaName === hostClassError.luaName);
        const target = document.querySelector('[data-host-class-error="' + index + '"]');
        if (target) target.textContent = hostClassError.message;
    }
}

function renderRunResponse(data) {
    hideAllResultPanels();
    renderHostClassErrorsInline(data.hostClassErrors);

    const status = byId('run-status');
    if (data.ok) {
        status.textContent = 'Completed';
        status.className = 'status-ok';
    } else {
        status.textContent = data.errorCategory || 'Failed';
        status.className = 'status-error';
        renderErrorPanel(data);
    }

    if (Array.isArray(data.returnValues)) {
        const valuesList = byId('values-list');
        clearChildren(valuesList);
        if (data.returnValues.length === 0) {
            valuesList.append(element('p', 'muted', '(no return values)'));
        }
        data.returnValues.forEach((value, index) => {
            const row = element('div', 'return-value');
            row.append(element('span', 'return-index', '[' + (index + 1) + ']'));
            row.append(renderValue(value));
            valuesList.append(row);
        });
        byId('values-panel').classList.remove('hidden');
    }

    if (typeof data.output === 'string' && data.output !== '') {
        byId('output-content').textContent = data.output;
        byId('output-truncated-note').classList.toggle('hidden', !(data.stats && data.stats.outputTruncated));
        byId('output-panel').classList.remove('hidden');
    }

    if (Array.isArray(data.callbackChunks) && data.callbackChunks.length > 0) {
        const chunksList = byId('chunks-list');
        clearChildren(chunksList);
        data.callbackChunks.forEach((chunkEntry, index) => {
            const wrapper = element('div');
            const badge = element('span', 'badge' + (chunkEntry.isStderr ? ' stderr' : ''),
                'chunk ' + (index + 1) + (chunkEntry.isStderr ? ' (stderr)' : ''));
            const pre = element('pre', null, chunkEntry.chunk);
            wrapper.append(badge, pre);
            chunksList.append(wrapper);
        });
        byId('chunks-panel').classList.remove('hidden');
    }

    renderWarnings(data);
    if (data.stats) renderStats(data.stats);
    renderProfile(data.profile);
}

function renderValidateResponse(data) {
    hideAllResultPanels();

    const status = byId('run-status');
    if (!data.ok) {
        status.textContent = data.errorCategory || 'Failed';
        status.className = 'status-error';
        renderErrorPanel(data);
        return;
    }
    const validationMessage = byId('validation-message');
    if (data.valid) {
        status.textContent = 'Valid';
        status.className = 'status-ok';
        validationMessage.textContent = 'The script compiles cleanly.';
        validationMessage.className = 'status-ok';
    } else {
        status.textContent = 'Invalid';
        status.className = 'status-error';
        validationMessage.textContent = (data.message || 'syntax error')
            + (data.line !== null && data.line !== undefined ? ' (line ' + data.line + ')' : '');
        validationMessage.className = 'error-inline';
    }
    byId('validation-panel').classList.remove('hidden');
}

// ---- VFS panel -------------------------------------------------------------

function renderVfsListing(listing) {
    const tableBody = byId('vfs-table-body');
    clearChildren(tableBody);
    byId('vfs-empty-note').classList.toggle('hidden', listing.length > 0);
    byId('vfs-table').classList.toggle('hidden', listing.length === 0);

    for (const fileEntry of listing) {
        const row = document.createElement('tr');
        row.append(element('td', null, fileEntry.path));
        row.append(element('td', null, fileEntry.size.toLocaleString() + ' B'));
        row.append(element('td', null, fileEntry.mtime ? new Date(fileEntry.mtime * 1000).toLocaleTimeString() : '—'));

        const actionsCell = element('td', 'row-actions');
        const viewButton = element('button', null, 'View');
        viewButton.addEventListener('click', () => viewVfsFile(fileEntry.path));
        actionsCell.append(viewButton);
        if (fileEntry.isText) {
            const editButton = element('button', null, 'Edit');
            editButton.addEventListener('click', () => editVfsFile(fileEntry.path));
            actionsCell.append(editButton);
        }
        const deleteButton = element('button', null, 'Delete');
        deleteButton.addEventListener('click', () => deleteVfsFile(fileEntry.path));
        actionsCell.append(deleteButton);
        row.append(actionsCell);
        tableBody.append(row);
    }
}

async function viewVfsFile(path) {
    const data = await callPlaygroundAction('vfsRead', { path });
    if (!data.ok) {
        byId('vfs-status').textContent = data.message || 'read failed';
        return;
    }
    byId('file-viewer-title').textContent = data.path;
    byId('file-viewer-meta').textContent = data.size.toLocaleString() + ' bytes, '
        + (data.kind === 'text' ? 'text' : 'binary (xxd view)')
        + (data.truncated ? ' — showing the first ' + (256 * 1024).toLocaleString() + ' bytes' : '');
    byId('file-viewer-content').textContent = data.kind === 'text' ? data.content : data.hexDump;
    byId('file-viewer').showModal();
}

async function editVfsFile(path) {
    const data = await callPlaygroundAction('vfsRead', { path });
    if (!data.ok || data.kind !== 'text') {
        byId('vfs-status').textContent = data.ok ? 'binary files cannot be edited inline' : (data.message || 'read failed');
        return;
    }
    byId('vfs-path').value = data.path;
    byId('vfs-content').value = data.content;
    byId('vfs-status').textContent = 'Editing ' + data.path + ' — Save file to apply.';
}

async function deleteVfsFile(path) {
    const data = await callPlaygroundAction('vfsDelete', { path });
    byId('vfs-status').textContent = data.ok ? 'Deleted ' + path : (data.message || 'delete failed');
}

async function saveVfsFile() {
    const path = byId('vfs-path').value.trim();
    if (path === '') {
        byId('vfs-status').textContent = 'Enter a path first (absolute, e.g. /notes.txt).';
        return;
    }
    const data = await callPlaygroundAction('vfsPut', { path, content: byId('vfs-content').value });
    byId('vfs-status').textContent = data.ok ? 'Saved ' + path : (data.message || 'save failed');
}

async function resetVfs() {
    const data = await callPlaygroundAction('resetVfs');
    byId('vfs-status').textContent = data.ok ? 'VFS reset.' : (data.message || 'reset failed');
}

// ---- Host classes panel ----------------------------------------------------

function renderHostClassEditor() {
    const listContainer = byId('host-class-list');
    clearChildren(listContainer);

    hostClasses.forEach((entry, index) => {
        const wrapper = element('div', 'host-class-entry');

        const nameRow = element('div', 'name-row');
        const nameLabel = element('label', null, 'Lua name');
        const nameInput = document.createElement('input');
        nameInput.type = 'text';
        nameInput.value = entry.luaName || '';
        nameInput.addEventListener('input', () => { entry.luaName = nameInput.value; persistState(); });
        const removeButton = element('button', null, 'Remove');
        removeButton.addEventListener('click', () => {
            hostClasses.splice(index, 1);
            renderHostClassEditor();
            persistState();
        });
        nameRow.append(nameLabel, nameInput, removeButton);

        const sourceEditor = document.createElement('textarea');
        sourceEditor.rows = 7;
        sourceEditor.spellcheck = false;
        sourceEditor.value = entry.phpSource || '';
        sourceEditor.addEventListener('input', () => { entry.phpSource = sourceEditor.value; persistState(); });

        const errorLine = element('div', 'error-inline host-class-error');
        errorLine.dataset.hostClassError = String(index);

        wrapper.append(nameRow, sourceEditor, errorLine);
        listContainer.append(wrapper);
    });
}

function addHostClassEntry() {
    hostClasses.push({ luaName: '', phpSource: 'return new class {\n};' });
    renderHostClassEditor();
    persistState();
}

// ---- Presets ---------------------------------------------------------------

function updatePresetDescription() {
    const preset = bootstrap.presets[byId('preset-select').value];
    byId('preset-description').textContent = preset ? preset.description : '';
}

async function applyPreset(presetId) {
    const preset = bootstrap.presets[presetId];
    if (!preset) return;

    byId('source-editor').value = preset.lua;
    for (const capabilityName of preset.requiredCapabilities || []) {
        const checkbox = byId('capability-' + capabilityName);
        if (checkbox) checkbox.checked = true;
    }
    for (const [path, contents] of Object.entries(preset.seedFiles || {})) {
        if (!lastVfsListing.some((fileEntry) => fileEntry.path === path)) {
            await callPlaygroundAction('vfsPut', { path, content: contents });
        }
    }
    updatePresetDescription();
    persistState();
}

// ---- Run / validate --------------------------------------------------------

function setBusy(isBusy) {
    byId('run-button').disabled = isBusy;
    byId('validate-button').disabled = isBusy;
    if (isBusy) {
        const status = byId('run-status');
        status.textContent = 'Running…';
        status.className = 'muted';
    }
}

async function runScript() {
    setBusy(true);
    try {
        renderRunResponse(await callPlaygroundAction('run'));
    } catch (requestError) {
        const status = byId('run-status');
        status.textContent = 'Request failed: ' + requestError.message;
        status.className = 'status-error';
    } finally {
        setBusy(false);
    }
}

async function validateScript() {
    setBusy(true);
    try {
        renderValidateResponse(await callPlaygroundAction('validate'));
    } catch (requestError) {
        const status = byId('run-status');
        status.textContent = 'Request failed: ' + requestError.message;
        status.className = 'status-error';
    } finally {
        setBusy(false);
    }
}

// ---- Init ------------------------------------------------------------------

function initializePlayground() {
    const meta = bootstrap.features;
    byId('header-meta').textContent = 'luaext ' + bootstrap.extensionVersion + ' running '
        + bootstrap.luaVersion + ' on ' + meta.platform
        + '. CPU limit ' + meta.cpuLimit + ', wall-clock limit ' + meta.wallClockLimit
        + ', cpu resolution ' + meta.cpuResolutionSeconds + 's.';
    byId('xdebug-banner').classList.toggle('hidden', !bootstrap.xdebugLoaded);
    byId('chunk-name-label').textContent = bootstrap.chunkName;
    byId('seconds-cap').textContent = String(bootstrap.runSecondsCap);

    const presetSelect = byId('preset-select');
    for (const [presetId, preset] of Object.entries(bootstrap.presets)) {
        const option = document.createElement('option');
        option.value = presetId;
        option.textContent = preset.label;
        presetSelect.append(option);
    }

    buildConfigInputs();
    applyStateToForm(loadState());
    renderVfsListing(lastVfsListing);

    presetSelect.addEventListener('change', () => applyPreset(presetSelect.value));
    byId('run-button').addEventListener('click', runScript);
    byId('validate-button').addEventListener('click', validateScript);
    byId('vfs-save-button').addEventListener('click', saveVfsFile);
    byId('vfs-reset-button').addEventListener('click', resetVfs);
    byId('host-class-add').addEventListener('click', addHostClassEntry);
    byId('deterministic').addEventListener('change', () => {
        byId('seed').disabled = !byId('deterministic').checked;
    });
    byId('capabilities-untrusted').addEventListener('click', () => {
        for (const definition of capabilityDefinitions) {
            byId('capability-' + definition.key).checked = definition.defaultValue;
        }
        persistState();
    });
    byId('capabilities-trusted').addEventListener('click', () => {
        for (const definition of capabilityDefinitions) {
            byId('capability-' + definition.key).checked = definition.trustedValue;
        }
        persistState();
    });

    // Any form interaction persists the whole state (cheap: it is one blob).
    document.body.addEventListener('change', persistState);
    let persistTimer = null;
    document.body.addEventListener('input', () => {
        clearTimeout(persistTimer);
        persistTimer = setTimeout(persistState, 400);
    });
}

initializePlayground();
</script>
</body>
</html>
PLAYGROUND_HTML;
}

// ---------------------------------------------------------------------------
// Dispatch — the only top-level executable statements in this file.
// ---------------------------------------------------------------------------

assertRequestIsFromLocalhost();

if (($_SERVER['REQUEST_METHOD'] ?? 'GET') === 'POST') {
    handlePostRequest();
} else {
    $playgroundFilesystem = PlaygroundFileSystem::loadFromSession();
    $playgroundFilesystem->saveToSession();
    renderPlaygroundPage($playgroundFilesystem);
}
