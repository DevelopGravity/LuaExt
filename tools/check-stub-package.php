<?php

declare(strict_types=1);

/*
 * check-stub-package.php — hold stubs/ to what the published stub package needs.
 *
 * stubs/ is two things at once: the arginfo source gen_stub.php reads, and the
 * root of developgravity/lua-ext-stubs, which release.yml publishes by running
 * `git subtree split --prefix=stubs`. A split carries only what is inside the
 * directory, so anything the package needs has to live there, and anything wrong
 * in there ships.
 *
 * The autoload check is the one that matters. These files declare real classes;
 * autoloading them in a project that also has the extension installed raises
 * "Cannot redeclare enum DevelopGravity\LuaExt\OutputMode" and takes the request
 * down. The package works only because it declares no autoload, which is an
 * invariant nothing else enforces -- Composer would happily accept the key.
 */

const REPO_ROOT = __DIR__ . '/..';

$failures = [];

/**
 * Record a failure with the file it belongs to.
 */
function fail(string $file, string $message): void
{
    global $failures;
    $failures[] = "$file: $message";
}

/**
 * Read a JSON file, or record why it could not be read.
 *
 * @return array<string, mixed>|null
 */
function readJson(string $path): ?array
{
    $relative = substr($path, strlen(REPO_ROOT) + 1);

    if (!is_file($path)) {
        fail($relative, 'missing.');

        return null;
    }

    $decoded = json_decode((string) file_get_contents($path), true);

    if (!is_array($decoded)) {
        fail($relative, 'is not valid JSON: ' . json_last_error_msg());

        return null;
    }

    return $decoded;
}

$stubManifest = readJson(REPO_ROOT . '/stubs/composer.json');
$rootManifest = readJson(REPO_ROOT . '/composer.json');

if ($stubManifest !== null) {
    if (array_key_exists('autoload', $stubManifest) || array_key_exists('autoload-dev', $stubManifest)) {
        fail(
            'stubs/composer.json',
            'declares an autoload section. These files declare real classes, so autoloading '
            . 'them alongside the installed extension is a fatal redeclare. Remove the key.'
        );
    }

    if (($stubManifest['name'] ?? null) !== 'developgravity/lua-ext-stubs') {
        fail('stubs/composer.json', 'name must be developgravity/lua-ext-stubs; README.md documents that name.');
    }

    if (($stubManifest['type'] ?? null) !== 'library') {
        fail('stubs/composer.json', 'type must be library. php-ext would make PIE try to build it.');
    }

    // A version key would fight the tag the release workflow pushes.
    if (array_key_exists('version', $stubManifest)) {
        fail('stubs/composer.json', 'must not carry a version key; the pushed tag is the version.');
    }

    if ($rootManifest !== null) {
        $stubPhp = $stubManifest['require']['php'] ?? null;
        $rootPhp = $rootManifest['require']['php'] ?? null;

        if ($stubPhp !== $rootPhp) {
            fail(
                'stubs/composer.json',
                sprintf('php constraint %s does not match the extension\'s %s.', var_export($stubPhp, true), var_export($rootPhp, true))
            );
        }
    }
}

/*
 * The split publishes whatever is in stubs/ and nothing else, so a package file
 * that only exists at the repo root never reaches the consumer.
 */
foreach (['README.md', 'LICENSE'] as $required) {
    if (!is_file(REPO_ROOT . "/stubs/$required")) {
        fail("stubs/$required", 'missing. The subtree split ships only stubs/, so the package would have no ' . $required . '.');
    }
}

$rootLicense = REPO_ROOT . '/LICENSE';
$stubLicense = REPO_ROOT . '/stubs/LICENSE';

if (is_file($rootLicense) && is_file($stubLicense) && file_get_contents($rootLicense) !== file_get_contents($stubLicense)) {
    fail('stubs/LICENSE', 'differs from the repository LICENSE. Copy it across; the package is MIT on the same terms.');
}

if ($failures !== []) {
    fwrite(STDERR, "check-stub-package: the published stub package would be wrong.\n\n");

    foreach ($failures as $failure) {
        fwrite(STDERR, "  - $failure\n");
    }

    fwrite(STDERR, "\n");
    exit(1);
}

echo "check-stub-package: stubs/ is publishable.\n";
