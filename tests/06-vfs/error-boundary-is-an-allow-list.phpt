--TEST--
Only a VfsError becomes a script-visible nil; any other backend failure is fatal
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

require __DIR__ . '/memory-filesystem.inc';

use DevelopGravity\LuaExt\Capabilities;
use DevelopGravity\LuaExt\Sandbox;
use DevelopGravity\LuaExt\SandboxConfig;

// The split is a security boundary, not a convenience.
//
// A VfsError is the backend saying something the script is entitled to hear --
// not found, refused, out of space -- so it arrives as `nil, message` and the
// script may handle it. Anything else means the HOST failed: a database down,
// a bug in the backend, an assertion. If those also became `nil`, a script
// could probe for backend outages and quietly swallow them, and the caller who
// could actually act on the fault would never learn about it.
//
// So the check is an exact allowlist. This test would pass just as happily
// against a catch-all `catch (Throwable)`, EXCEPT for the second half.

$make = static function (array $refuseOn, array $failOn): Sandbox {
	$backend = new MemoryFileSystem(['/data.txt' => 'contents']);
	$backend->refuseOn = $refuseOn;
	$backend->failOn = $failOn;

	return new Sandbox(new SandboxConfig(
		capabilities: (new Capabilities())->with(vfs: true, vfsWrite: true),
		filesystem: $backend,
	));
};

// Rendered explicitly rather than through print_r, which writes "[0] => " with
// a trailing space for null -- an expectation any editor silently strips.
$show = static function (string $label, array $returned): void {
	printf("%-9s %s | %s\n", $label, var_export($returned[0], true), var_export($returned[1], true));
};

// A VfsError: catchable, and the message survives.
$sandbox = $make(['read'], []);
$show('io.open', $sandbox->eval('local f, err = io.open("/data.txt", "r") return f, err', '=vfs'));
$sandbox->close();

// os.remove uses the same route and reports the same way.
$sandbox = $make(['delete'], []);
$show('os.remove', $sandbox->eval('local ok, err = os.remove("/data.txt") return ok, err', '=vfs'));
$sandbox->close();

// A host failure: NOT catchable, and it reaches the caller as the class the
// backend threw rather than degraded to a string or turned into nil.
$sandbox = $make([], ['read']);

try {
	$result = $sandbox->eval('local f, err = io.open("/data.txt", "r") return "swallowed: " .. tostring(err)', '=vfs');
	printf("HOST FAILURE ESCAPED: %s\n", var_export($result, true));
} catch (Throwable $error) {
	printf("%s: %s\n", $error::class, $error->getMessage());
}

$sandbox->close();

// And a script cannot catch it from inside Lua either -- pcall must not turn a
// host fault into a value the script gets to ignore.
$sandbox = $make([], ['read']);

try {
	$result = $sandbox->eval('local ok = pcall(io.open, "/data.txt", "r") return "swallowed: " .. tostring(ok)', '=vfs');
	printf("PCALL SWALLOWED IT: %s\n", var_export($result, true));
} catch (Throwable $error) {
	printf("%s survives pcall\n", $error::class);
}

$sandbox->close();

?>
--EXPECT--
io.open   NULL | 'read refused for /data.txt'
os.remove NULL | 'delete refused for /data.txt'
RuntimeException: backend exploded during read
RuntimeException survives pcall
