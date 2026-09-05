<?php

/**
 * check-playground-sync.php — refuse a config surface the playground cannot drive.
 *
 * The playground is the first place a person meets a new Limits/VfsQuota field
 * or Capabilities flag, and it drifted the very first time one shipped:
 * billHostTime landed in the extension and the panel knew nothing about it,
 * which surfaced as a user report rather than a red gate. This compares the
 * stubs' constructor parameters — the authoritative list of what a host can
 * configure — against the playground's client-side definition arrays and its
 * server-side payload readers, and fails on any parameter missing from either.
 *
 * A parameter the playground deliberately does not expose goes in the allow
 * list below with its reason, so an omission is a decision on record rather
 * than an accident.
 *
 * Run with `php -n`: the stubs cannot be declared while a system-installed
 * luaext is loaded, exactly as tools/check-docs-api.php explains.
 */

declare(strict_types=1);

const STUB_FILE = __DIR__ . '/../stubs/luaext.stub.php';
const PLAYGROUND_FILE = __DIR__ . '/../examples/playground/index.php';

/** @var array<string, array<string, string>> parameter => why it is absent */
const DELIBERATELY_ABSENT = [
    'Limits' => [],
    'VfsQuota' => [],
    'Capabilities' => [
        'osEnvAllowList' => 'a free-text field beside the grid, not a checkbox definition',
    ],
];

if (extension_loaded('luaext')) {
    fwrite(STDERR, "check-playground-sync: run with `php -n` -- the stubs cannot be "
        . "declared while a loaded luaext provides the same classes.\n");
    exit(2);
}

require STUB_FILE;

/** @return list<string> the constructor's parameter names */
function constructorParameters(string $class): array
{
    $names = [];

    foreach ((new ReflectionClass($class))->getConstructor()->getParameters() as $parameter) {
        $names[] = $parameter->getName();
    }

    return $names;
}

$playground = file_get_contents(PLAYGROUND_FILE);

if ($playground === false) {
    fwrite(STDERR, "check-playground-sync: cannot read the playground.\n");
    exit(2);
}

$failures = [];

$surfaces = [
    'Limits' => DevelopGravity\LuaExt\Limits::class,
    'VfsQuota' => DevelopGravity\LuaExt\VfsQuota::class,
    'Capabilities' => DevelopGravity\LuaExt\Capabilities::class,
];

foreach ($surfaces as $label => $class) {
    foreach (constructorParameters($class) as $parameter) {
        if (isset(DELIBERATELY_ABSENT[$label][$parameter])) {
            continue;
        }

        // Client side: a definition entry ({ key: 'x' ...) or a dedicated
        // element id (limit-x / quota-x / capability-x), covering both the
        // grid-built inputs and the hand-placed checkboxes.
        $inClient = str_contains($playground, sprintf("key: '%s'", $parameter))
            || preg_match(sprintf('/id="(?:limit|quota|capability)-%s"/', preg_quote($parameter, '/')), $playground) === 1;

        // Server side: the payload reader names the key somewhere.
        $inServer = str_contains($playground, sprintf("'%s'", $parameter))
            || str_contains($playground, sprintf('%s:', $parameter));

        if (!$inClient) {
            $failures[] = sprintf('%s::$%s has no playground control (no definition entry or input id)', $label, $parameter);
        }

        if (!$inServer) {
            $failures[] = sprintf('%s::$%s is never read from the playground payload', $label, $parameter);
        }
    }
}

if ($failures !== []) {
    fwrite(STDERR, "check-playground-sync: the playground has drifted from the config surface.\n\n");

    foreach ($failures as $failure) {
        fwrite(STDERR, '  ' . $failure . "\n");
    }

    fwrite(STDERR, "\nAdd the control (and server reader), or record the omission in "
        . "DELIBERATELY_ABSENT with its reason.\n");
    exit(1);
}

echo "check-playground-sync: ok -- every config parameter is drivable from the playground.\n";
