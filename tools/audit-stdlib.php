#!/usr/bin/env php
<?php

/**
 * Audits the Lua standard-library surface a sandbox exposes, and pins it to
 * golden files under tests/golden/stdlib/.
 *
 * The point is not the files. The point is the direction of failure when the
 * vendored Lua tree is bumped:
 *
 *   Lua 5.5.2 adds string.frobnicate. Nothing in src/ mentions it, so no
 *   allow-list changes, so nothing in the extension notices. This tool notices:
 *   upstream-members.txt gains a line, the committed golden does not match, and
 *   `tools/check-generated.sh` fails until somebody classifies the member.
 *
 * The same mechanism catches the opposite mistake. If a capability quietly
 * starts exposing something it should not, the per-preset surface changes and
 * the diff reads as "this capability changed exactly these names".
 *
 * Three golden files:
 *
 *   exposed.txt           every name reachable from _G, per capability preset,
 *                         plus two probes nothing reachable from _G would catch
 *   upstream-members.txt  the luaL_Reg arrays in the vendored Lua sources
 *   withheld.txt          upstream members no preset can reach, at any level
 *
 * Usage:
 *   php tools/audit-stdlib.php            regenerate the golden files
 *   php tools/audit-stdlib.php --check    verify them; exit 1 on any difference
 *
 * The exposed surface has to be measured against a real interpreter, so this
 * needs the built extension. It finds modules/luaext.so on its own when the
 * extension is not already loaded. Where it cannot be found, the upstream
 * tripwire still runs and the two interpreter-derived files are reported --
 * loudly -- as unaudited, because a check that quietly passes is the exact
 * failure this tool exists to prevent.
 */

declare(strict_types=1);

namespace DevelopGravity\LuaExt\Tools\AuditStdlib;

use DevelopGravity\LuaExt\Capabilities;
use DevelopGravity\LuaExt\Limits;
use DevelopGravity\LuaExt\Sandbox;
use DevelopGravity\LuaExt\SandboxConfig;
use Throwable;

/** What the tool was asked to do. */
enum Mode
{
    /** Rewrite the golden files from the current sources and interpreter. */
    case Regenerate;

    /** Re-derive into memory and fail on any difference from what is committed. */
    case Check;
}

/** Whether a golden file matched, differed, or could not be produced at all. */
enum Outcome
{
    case Matched;
    case Written;
    case Differed;
    case Missing;
    case Unaudited;
}

const REPOSITORY_ROOT = __DIR__ . '/..';
const GOLDEN_DIRECTORY = REPOSITORY_ROOT . '/tests/golden/stdlib';
const VENDORED_LUA_SOURCE = REPOSITORY_ROOT . '/third_party/lua-5.5.1/src';

const EXPOSED_GOLDEN = 'exposed.txt';
const UPSTREAM_GOLDEN = 'upstream-members.txt';
const WITHHELD_GOLDEN = 'withheld.txt';

/** Set when this process already re-executed itself with the extension loaded. */
const REEXEC_GUARD_VARIABLE = 'LUAEXT_AUDIT_STDLIB_REEXEC';

/**
 * The luaL_Reg arrays in the vendored sources, and the table each one becomes.
 *
 * Explicit rather than discovered, so a NEW array in one of these files is a
 * hard failure naming the file to classify it in, exactly like an unknown
 * member. A prefix of null means the array is not a library table at all -- the
 * string metatable's arithmetic metamethods -- and is recorded separately so it
 * never lands in the withheld arithmetic as a phantom `string.__add`.
 *
 * lcorolib.c is here even though nothing opens it: it is compiled into the
 * binary, and the coroutine wrapper that will select from it needs its member
 * list pinned before it is written, not after.
 *
 * @var array<string, array<string, string|null>>
 */
const UPSTREAM_REGISTRIES = [
    'lbaselib.c' => ['base_funcs' => ''],
    'lstrlib.c' => ['strlib' => 'string.', 'stringmetamethods' => null],
    'ltablib.c' => ['tab_funcs' => 'table.'],
    'lmathlib.c' => ['mathlib' => 'math.', 'randfuncs' => 'math.'],
    'lutf8lib.c' => ['funcs' => 'utf8.'],
    'ldblib.c' => ['dblib' => 'debug.'],
    'lcorolib.c' => ['co_funcs' => 'coroutine.'],
];

/**
 * Walks _G with next/rawget only.
 *
 * No metamethods are involved anywhere in this chunk, so a hostile __index
 * cannot make the audit report a surface that is not there -- or hide one that
 * is. _ENV rather than _G for the same reason plus one more: _ENV is a lexical
 * upvalue, so the walk still works in a preset where the _G global itself has
 * been withheld.
 */
const SURFACE_CHUNK = <<<'LUA'
    local environment = _ENV
    local result = {}

    local function record(prefix, subject)
        local key = next(subject)

        while key ~= nil do
            if type(key) == "string" then
                result[#result + 1] = prefix .. key .. "\t" .. type(rawget(subject, key))
            end

            key = next(subject, key)
        end
    end

    record("", environment)

    -- One level down, into the library tables published as globals. Anything
    -- deeper is not stdlib surface, and _ENV is skipped so that _G._G does not
    -- report every global a second time.
    local key = next(environment)

    while key ~= nil do
        if type(key) == "string" then
            local value = rawget(environment, key)

            if type(value) == "table" and value ~= environment then
                record(key .. ".", value)
            end
        end

        key = next(environment, key)
    end

    return result
    LUA;

/**
 * Two facts about the exposed surface that walking _G cannot reach.
 *
 * The string metatable is reachable only through getmetatable(""), and it is
 * what makes ("x"):upper() work -- so a preset that withholds the string
 * library while leaving the metatable wired is a hole nothing else would show.
 * The collectgarbage verbs live behind a single name, so the whole gcControl
 * capability is invisible to a name-based audit.
 */
const PROBE_CHUNK = <<<'LUA'
    local environment = _ENV
    local report = {}

    local getmetatable = rawget(environment, "getmetatable")
    local collectgarbage = rawget(environment, "collectgarbage")
    local pcall = rawget(environment, "pcall")

    if getmetatable == nil then
        report[#report + 1] = 'getmetatable("").__index == string\tno getmetatable'
    else
        local metatable = getmetatable("")

        if metatable == nil then
            report[#report + 1] = 'getmetatable("").__index == string\tno metatable'
        else
            local index = rawget(metatable, "__index")

            report[#report + 1] = 'getmetatable("").__index == string\t'
                .. tostring(index ~= nil and index == rawget(environment, "string"))
            report[#report + 1] = 'getmetatable("").__index type\t' .. type(index)
        end
    end

    if collectgarbage == nil or pcall == nil then
        report[#report + 1] = 'collectgarbage\tabsent'
    else
        -- "stop" before "restart" so the sandbox is not left with its collector
        -- switched off, and an unknown verb last to show refusal still works.
        local verbs = {
            "count", "step", "isrunning", "incremental", "generational",
            "collect", "stop", "restart", "nonesuch",
        }

        for index = 1, #verbs do
            local verb = verbs[index]
            local ok = pcall(collectgarbage, verb)

            report[#report + 1] = 'collectgarbage("' .. verb .. '")\t' .. (ok and "ok" or "refused")
        end

        -- "param" needs a parameter name, so probing it bare would report
        -- "refused" for the wrong reason.
        local ok = pcall(collectgarbage, "param", "pause")

        report[#report + 1] = 'collectgarbage("param", "pause")\t' .. (ok and "ok" or "refused")
    end

    return report
    LUA;

/* -------------------------------------------------------------------------
 * Presets
 * ---------------------------------------------------------------------- */

/**
 * Every capability preset the audit measures.
 *
 * Three named presets plus one per single-capability delta from the untrusted
 * baseline. The deltas are the reason the golden is worth reading: each one
 * differs from `untrusted` by exactly one flag, so a diff on this file answers
 * "what does granting debugIntrospect actually add?" without anybody having to
 * reason about it.
 *
 * @return array<string, Capabilities>
 */
function buildPresets(): array
{
    $untrusted = Capabilities::untrusted();

    $presets = [
        // Nothing at all: the floor a sandbox can be configured down to.
        'minimal' => new Capabilities(
            coroutines: false,
            osTime: false,
            debugTraceback: false,
            utf8: false,
        ),
        'untrusted' => $untrusted,

        /*
         * Capabilities::trusted() as a sandbox can actually be built: it grants
         * vfs, and luaext_config_resolve refuses that without a FileSystem to
         * back it. Every preset here is constructed with the same empty
         * filesystem, so the name records the intent rather than a difference.
         */
        'trusted-constructible' => Capabilities::trusted(),
    ];

    foreach (capabilityFlags() as $flag) {
        $enabled = $untrusted->$flag;
        $sign = $enabled ? '-' : '+';

        $presets["untrusted{$sign}{$flag}"] = $untrusted->with(...[$flag => !$enabled]);
    }

    return $presets;
}

/**
 * The boolean capability flags, in declaration order.
 *
 * Reflected rather than listed so that a capability added to the stub cannot be
 * left out of the audit by forgetting to add it here.
 *
 * @return list<string>
 */
function capabilityFlags(): array
{
    $flags = [];

    foreach ((new \ReflectionClass(Capabilities::class))->getProperties() as $property) {
        if ($property->getType()?->getName() === 'bool') {
            $flags[] = $property->getName();
        }
    }

    return $flags;
}

/**
 * A FileSystem with nothing in it.
 *
 * Present only so the vfs capability is constructible; the audit never reads or
 * writes through it, and it deliberately implements the interface as narrowly as
 * possible so that it cannot widen the surface it is measuring.
 */
function emptyFileSystem(): \DevelopGravity\LuaExt\FileSystem
{
    return new class implements \DevelopGravity\LuaExt\FileSystem {
        public function exists(string $path): bool
        {
            return false;
        }

        public function stat(string $path): ?\DevelopGravity\LuaExt\FileStat
        {
            return null;
        }

        public function read(string $path): string
        {
            throw new \DevelopGravity\LuaExt\Exception\VfsError('empty filesystem');
        }

        public function write(string $path, string $contents): void
        {
            throw new \DevelopGravity\LuaExt\Exception\VfsError('empty filesystem');
        }

        public function delete(string $path): void
        {
            throw new \DevelopGravity\LuaExt\Exception\VfsError('empty filesystem');
        }

        public function rename(string $from, string $to): void
        {
            throw new \DevelopGravity\LuaExt\Exception\VfsError('empty filesystem');
        }

        public function list(string $path): array
        {
            return [];
        }
    };
}

/**
 * Measure one preset's exposed surface.
 *
 * @return array{names: list<string>, probes: list<string>, error: ?string}
 */
function measurePreset(Capabilities $capabilities): array
{
    try {
        /*
         * BOTH time limits are lifted, for every preset rather than only the
         * debugHooks one, so that varying limits do not put an irrelevant
         * difference in the golden.
         *
         * Both, not just cpuSeconds: debugHooks is refused alongside either,
         * because a script calling debug.sethook() displaces the interpreter
         * hook they are both delivered through. Lifting only the CPU limit made
         * that preset throw, and a preset that throws contributes no names --
         * so debug.sethook and debug.gethook silently appeared in withheld.txt
         * as though no configuration could reach them. A security audit whose
         * failure mode is under-reporting exposure is worse than no audit.
         */
        $sandbox = new Sandbox(new SandboxConfig(
            capabilities: $capabilities,
            limits: new Limits(cpuSeconds: null, wallClockSeconds: null),
            filesystem: emptyFileSystem(),
        ));
    } catch (Throwable $error) {
        return ['names' => [], 'probes' => [], 'error' => $error::class . ': ' . $error->getMessage()];
    }

    try {
        $names = $sandbox->eval(SURFACE_CHUNK, '=audit-stdlib(surface)')[0] ?? [];
        $probes = $sandbox->eval(PROBE_CHUNK, '=audit-stdlib(probes)')[0] ?? [];
    } catch (Throwable $error) {
        return ['names' => [], 'probes' => [], 'error' => $error::class . ': ' . $error->getMessage()];
    } finally {
        $sandbox->close();
    }

    $names = array_values(array_unique(array_map(strval(...), (array) $names)));
    $probes = array_values(array_unique(array_map(strval(...), (array) $probes)));

    sort($names, SORT_STRING);
    sort($probes, SORT_STRING);

    return ['names' => $names, 'probes' => $probes, 'error' => null];
}

/* -------------------------------------------------------------------------
 * Rendering
 * ---------------------------------------------------------------------- */

/**
 * The header every golden file carries, so nobody edits one by hand.
 */
function header(string $title, string $explanation): string
{
    $lines = [
        '# GENERATED FILE — do not edit by hand.',
        '#',
        '# ' . $title,
        '#',
        '# Regenerate:  php tools/audit-stdlib.php',
        '# Verify:      php tools/audit-stdlib.php --check   (via tools/check-generated.sh)',
        '#',
    ];

    foreach (explode("\n", $explanation) as $line) {
        $lines[] = rtrim('# ' . $line);
    }

    $lines[] = '';

    return implode("\n", $lines) . "\n";
}

/**
 * @param array<string, array{names: list<string>, probes: list<string>, error: ?string}> $measurements
 */
function renderExposed(array $measurements): string
{
    $explanation = <<<'TEXT'
        Every name a script can reach from _G, one per line as `name<TAB>luatype`,
        for each capability preset. Nested names are one level deep: `string.byte`
        is stdlib surface, `package.loaded.string.byte` would not be.

        The walk uses next() and rawget() only, so no metamethod runs and a
        hostile __index cannot influence what is reported.

        The `probes` block records the two things a name walk cannot see: what the
        string metatable's __index points at, and which collectgarbage verbs are
        actually permitted.
        TEXT;

    $output = header('The exposed standard-library surface, per capability preset.', $explanation);

    foreach ($measurements as $preset => $measurement) {
        $output .= "\n[preset {$preset}]\n";

        if ($measurement['error'] !== null) {
            $output .= "!unconstructible\t{$measurement['error']}\n";
            continue;
        }

        foreach ($measurement['names'] as $line) {
            $output .= $line . "\n";
        }

        $output .= "\n[preset {$preset} probes]\n";

        foreach ($measurement['probes'] as $line) {
            $output .= $line . "\n";
        }
    }

    return $output;
}

/**
 * @param array{members: list<string>, metamethods: list<string>} $upstream
 */
function renderUpstream(array $upstream): string
{
    $explanation = <<<'TEXT'
        Every member named by a luaL_Reg array in the vendored Lua sources.

        This is the tripwire that fires from the build rather than from review: a
        Lua point release that adds a member changes this file, the committed
        golden no longer matches, and the drift check fails until the member is
        classified in the matching allow-list under src/.

        Preprocessor conditionals inside an array are ignored, so members behind a
        LUA_COMPAT_* guard that is not defined for this build are still listed.
        That is the conservative direction: they show up as withheld rather than
        going unmentioned.

        lcorolib.c is included although nothing opens it. It is compiled in, and
        the coroutine wrapper that will select from it needs its list pinned now.

        The metamethods block is the string metatable's arithmetic operators.
        They are not members of any library table, so they are listed separately
        and take no part in the withheld arithmetic.
        TEXT;

    $output = header('Members declared by the vendored Lua standard library.', $explanation);

    $output .= "\n[members]\n";

    foreach ($upstream['members'] as $member) {
        $output .= $member . "\n";
    }

    $output .= "\n[string metamethods]\n";

    foreach ($upstream['metamethods'] as $metamethod) {
        $output .= $metamethod . "\n";
    }

    return $output;
}

/**
 * @param list<string> $withheld
 */
function renderWithheld(array $withheld): string
{
    $explanation = <<<'TEXT'
        Upstream members minus the union of every preset's exposed set: what no
        configuration of this extension can reach, at any capability level.

        Read it as the permanent record that debug.debug, dofile and loadfile are
        unreachable however a sandbox is configured — not merely off by default.

        A name LEAVING this file is the interesting direction. It means something
        that was unreachable at every level has become reachable at some level,
        which is a deliberate act or a bug, and either way is worth a review
        comment.
        TEXT;

    $output = header('Upstream members no capability level exposes.', $explanation);

    $output .= "\n";

    foreach ($withheld as $member) {
        $output .= $member . "\n";
    }

    return $output;
}

/* -------------------------------------------------------------------------
 * Parsing the vendored sources
 * ---------------------------------------------------------------------- */

/**
 * Extract the luaL_Reg member names from the vendored Lua sources.
 *
 * @return array{members: list<string>, metamethods: list<string>}
 */
function parseUpstreamMembers(): array
{
    $members = [];
    $metamethods = [];

    foreach (UPSTREAM_REGISTRIES as $file => $registries) {
        $path = VENDORED_LUA_SOURCE . '/' . $file;
        $source = @file_get_contents($path);

        if ($source === false) {
            fail("cannot read the vendored source {$path}");
        }

        $found = [];

        if (preg_match_all(
            '/luaL_Reg\s+(\w+)\s*\[\s*\]\s*=\s*\{(.*?)\n\s*\}\s*;/s',
            $source,
            $matches,
            PREG_SET_ORDER,
        ) === false) {
            fail("cannot scan {$path} for luaL_Reg arrays");
        }

        foreach ($matches as $match) {
            [, $arrayName, $body] = $match;
            $found[$arrayName] = true;

            if (!array_key_exists($arrayName, $registries)) {
                fail(
                    "third_party/lua-5.5.1/src/{$file} declares a luaL_Reg array "
                    . "'{$arrayName}' that tools/audit-stdlib.php does not know about. "
                    . 'Add it to UPSTREAM_REGISTRIES with the table it becomes, or null '
                    . 'if it is not a library table.',
                );
            }

            $prefix = $registries[$arrayName];

            preg_match_all('/\{\s*"((?:[^"\\\\]|\\\\.)*)"\s*,/', $body, $entries);

            foreach ($entries[1] as $name) {
                if ($prefix === null) {
                    $metamethods[] = 'string(metatable).' . $name;
                } else {
                    $members[] = $prefix . $name;
                }
            }
        }

        foreach (array_keys($registries) as $expected) {
            if (!isset($found[$expected])) {
                fail(
                    "third_party/lua-5.5.1/src/{$file} no longer declares the luaL_Reg "
                    . "array '{$expected}'. It was renamed or removed upstream; "
                    . 'update UPSTREAM_REGISTRIES in tools/audit-stdlib.php.',
                );
            }
        }
    }

    $members = array_values(array_unique($members));
    $metamethods = array_values(array_unique($metamethods));

    sort($members, SORT_STRING);
    sort($metamethods, SORT_STRING);

    return ['members' => $members, 'metamethods' => $metamethods];
}

/* -------------------------------------------------------------------------
 * Golden-file handling
 * ---------------------------------------------------------------------- */

/**
 * Write the golden, or compare against it and report the difference.
 */
function reconcile(string $name, string $content, Mode $mode): Outcome
{
    $path = GOLDEN_DIRECTORY . '/' . $name;

    if ($mode === Mode::Regenerate) {
        if (!is_dir(GOLDEN_DIRECTORY) && !mkdir(GOLDEN_DIRECTORY, 0o755, true) && !is_dir(GOLDEN_DIRECTORY)) {
            fail('cannot create ' . GOLDEN_DIRECTORY);
        }

        if (file_put_contents($path, $content) === false) {
            fail("cannot write {$path}");
        }

        return Outcome::Written;
    }

    if (!is_file($path)) {
        fwrite(STDERR, "audit-stdlib: tests/golden/stdlib/{$name} is missing.\n");
        fwrite(STDERR, "audit-stdlib: run `php tools/audit-stdlib.php` to create them.\n");

        return Outcome::Missing;
    }

    $committed = file_get_contents($path);

    if ($committed === $content) {
        return Outcome::Matched;
    }

    fwrite(STDERR, unifiedDiff("tests/golden/stdlib/{$name}", (string) $committed, $content));

    return Outcome::Differed;
}

/**
 * A unified diff of two texts.
 *
 * Common prefix and suffix are trimmed before the O(n*m) table is built, which
 * is what keeps this usable on a golden file thousands of lines long: the part
 * that actually differs is almost always small.
 */
function unifiedDiff(string $label, string $before, string $after): string
{
    $beforeLines = explode("\n", $before);
    $afterLines = explode("\n", $after);

    $head = 0;
    $beforeCount = count($beforeLines);
    $afterCount = count($afterLines);

    while ($head < $beforeCount && $head < $afterCount && $beforeLines[$head] === $afterLines[$head]) {
        $head++;
    }

    $tail = 0;

    while (
        $tail < $beforeCount - $head
        && $tail < $afterCount - $head
        && $beforeLines[$beforeCount - 1 - $tail] === $afterLines[$afterCount - 1 - $tail]
    ) {
        $tail++;
    }

    $beforeMiddle = array_slice($beforeLines, $head, $beforeCount - $head - $tail);
    $afterMiddle = array_slice($afterLines, $head, $afterCount - $head - $tail);

    $output = "--- {$label} (committed)\n+++ {$label} (regenerated)\n";
    $output .= sprintf(
        "@@ -%d,%d +%d,%d @@\n",
        $head + 1,
        count($beforeMiddle),
        $head + 1,
        count($afterMiddle),
    );

    foreach (longestCommonSubsequenceDiff($beforeMiddle, $afterMiddle) as $line) {
        $output .= $line . "\n";
    }

    return $output;
}

/**
 * Diff two line lists via their longest common subsequence.
 *
 * @param list<string> $before
 * @param list<string> $after
 *
 * @return list<string>
 */
function longestCommonSubsequenceDiff(array $before, array $after): array
{
    $beforeCount = count($before);
    $afterCount = count($after);

    // A pathological diff is not worth a gigabyte of table; fall back to a
    // whole-block replacement, which is still a correct unified diff.
    if ($beforeCount * $afterCount > 4_000_000) {
        return array_merge(
            array_map(static fn (string $line): string => '-' . $line, $before),
            array_map(static fn (string $line): string => '+' . $line, $after),
        );
    }

    $lengths = array_fill(0, $beforeCount + 1, array_fill(0, $afterCount + 1, 0));

    for ($i = $beforeCount - 1; $i >= 0; $i--) {
        for ($j = $afterCount - 1; $j >= 0; $j--) {
            $lengths[$i][$j] = $before[$i] === $after[$j]
                ? $lengths[$i + 1][$j + 1] + 1
                : max($lengths[$i + 1][$j], $lengths[$i][$j + 1]);
        }
    }

    $result = [];
    $i = 0;
    $j = 0;

    while ($i < $beforeCount && $j < $afterCount) {
        if ($before[$i] === $after[$j]) {
            $result[] = ' ' . $before[$i];
            $i++;
            $j++;
        } elseif ($lengths[$i + 1][$j] >= $lengths[$i][$j + 1]) {
            $result[] = '-' . $before[$i];
            $i++;
        } else {
            $result[] = '+' . $after[$j];
            $j++;
        }
    }

    while ($i < $beforeCount) {
        $result[] = '-' . $before[$i];
        $i++;
    }

    while ($j < $afterCount) {
        $result[] = '+' . $after[$j];
        $j++;
    }

    return $result;
}

/* -------------------------------------------------------------------------
 * Entry point
 * ---------------------------------------------------------------------- */

/**
 * Abort with a message. Never exit 0: a failed audit that looks like a pass is
 * worse than no audit.
 */
function fail(string $message): never
{
    fwrite(STDERR, "audit-stdlib: {$message}\n");
    exit(1);
}

/**
 * Make the extension available, re-executing this script once if that is what
 * it takes. Returns false when it genuinely cannot be found.
 */
function ensureExtensionLoaded(array $argv): bool
{
    if (extension_loaded('luaext')) {
        return true;
    }

    if (getenv(REEXEC_GUARD_VARIABLE) !== false) {
        return false;
    }

    foreach (['/modules/luaext.so', '/modules/luaext.dll', '/.libs/luaext.so'] as $candidate) {
        $path = REPOSITORY_ROOT . $candidate;

        if (!is_file($path)) {
            continue;
        }

        $command = array_merge(
            [PHP_BINARY, '-d', 'extension=' . realpath($path), __FILE__],
            array_slice($argv, 1),
        );

        $escaped = implode(' ', array_map(escapeshellarg(...), $command));

        putenv(REEXEC_GUARD_VARIABLE . '=1');
        passthru($escaped, $status);

        exit($status);
    }

    return false;
}

/**
 * @param list<string> $argv
 */
function main(array $argv): int
{
    $mode = Mode::Regenerate;

    foreach (array_slice($argv, 1) as $argument) {
        if ($argument === '--check') {
            $mode = Mode::Check;
            continue;
        }

        fwrite(STDERR, "usage: php tools/audit-stdlib.php [--check]\n");

        return 2;
    }

    $haveExtension = ensureExtensionLoaded($argv);
    $outcomes = [];

    $upstream = parseUpstreamMembers();
    $outcomes[UPSTREAM_GOLDEN] = reconcile(UPSTREAM_GOLDEN, renderUpstream($upstream), $mode);

    if (!$haveExtension) {
        /*
         * The upstream tripwire is source-only and just ran. The other two need
         * a real interpreter, so they are reported as unaudited by name -- never
         * as passing, and never silently.
         */
        fwrite(STDERR, "\n");
        fwrite(STDERR, "audit-stdlib: the luaext extension is not loaded and modules/luaext.so was not found.\n");
        fwrite(STDERR, "audit-stdlib: NOT AUDITED: " . EXPOSED_GOLDEN . ', ' . WITHHELD_GOLDEN . "\n");
        fwrite(STDERR, "audit-stdlib: build the extension (phpize && ./configure && make) and re-run,\n");
        fwrite(STDERR, "audit-stdlib: or run with -d extension=/path/to/luaext.so.\n");

        $outcomes[EXPOSED_GOLDEN] = Outcome::Unaudited;
        $outcomes[WITHHELD_GOLDEN] = Outcome::Unaudited;

        if ($mode === Mode::Regenerate) {
            fail('refusing to write a partial set of golden files');
        }

        return report($outcomes, $mode);
    }

    $measurements = [];
    $exposed = [];

    foreach (buildPresets() as $preset => $capabilities) {
        $measurement = measurePreset($capabilities);
        $measurements[$preset] = $measurement;

        foreach ($measurement['names'] as $line) {
            $exposed[substr($line, 0, (int) strpos($line, "\t"))] = true;
        }
    }

    $withheld = array_values(array_filter(
        $upstream['members'],
        static fn (string $member): bool => !isset($exposed[$member]),
    ));

    sort($withheld, SORT_STRING);

    $outcomes[EXPOSED_GOLDEN] = reconcile(EXPOSED_GOLDEN, renderExposed($measurements), $mode);
    $outcomes[WITHHELD_GOLDEN] = reconcile(WITHHELD_GOLDEN, renderWithheld($withheld), $mode);

    return report($outcomes, $mode);
}

/**
 * @param array<string, Outcome> $outcomes
 */
function report(array $outcomes, Mode $mode): int
{
    $status = 0;

    foreach ($outcomes as $name => $outcome) {
        $label = match ($outcome) {
            Outcome::Matched => 'ok',
            Outcome::Written => 'written',
            Outcome::Differed => 'DRIFTED',
            Outcome::Missing => 'MISSING',
            Outcome::Unaudited => 'not audited',
        };

        printf("%-24s %s\n", $name, $label);

        if ($outcome === Outcome::Differed || $outcome === Outcome::Missing) {
            $status = 1;
        }
    }

    if ($status === 0 && $mode === Mode::Check) {
        echo in_array(Outcome::Unaudited, $outcomes, true)
            ? "audit-stdlib: everything that COULD be audited matches; see the warnings above.\n"
            : "audit-stdlib: the stdlib surface matches the golden files.\n";
    }

    return $status;
}

exit(main($argv));
