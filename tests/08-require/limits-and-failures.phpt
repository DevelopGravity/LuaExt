--TEST--
require() bounds depth and count, refuses a cycle, and does not cache a failure
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

require __DIR__ . '/../06-vfs/memory-filesystem.inc';

use DevelopGravity\LuaExt\Capabilities;
use DevelopGravity\LuaExt\Exception\FatalError;
use DevelopGravity\LuaExt\Limits;
use DevelopGravity\LuaExt\Sandbox;
use DevelopGravity\LuaExt\SandboxConfig;

$make = static function (array $files, ?Limits $limits = null): Sandbox {
	return new Sandbox(new SandboxConfig(
		limits: $limits ?? new Limits(),
		capabilities: (new Capabilities())->with(require: true, vfs: true),
		filesystem: new MemoryFileSystem($files),
	));
};

// A cycle fails cleanly instead of recursing until the C stack gives out. The
// guard is a separate table from package.loaded precisely so that "in progress"
// and "already loaded" stay distinguishable.
$sandbox = $make([
	'/a.lua' => 'require("b") return {}',
	'/b.lua' => 'require("a") return {}',
]);

try {
	(void) $sandbox->eval('require("a")', '=cycle');
	echo "cycle: NOT DETECTED\n";
} catch (Throwable $error) {
	printf("cycle: %s\n", $error->getMessage());
}

$sandbox->close();

// Depth is bounded independently of the cycle guard: a chain of distinct
// modules is not circular, and would otherwise nest as deep as it liked.
$chain = [];

for ($index = 0; $index < 12; $index++) {
	$chain[sprintf('/m%d.lua', $index)] = sprintf('require("m%d") return {}', $index + 1);
}

$chain['/m12.lua'] = 'return {}';

$sandbox = $make($chain, new Limits(maxRequireDepth: 4));

try {
	(void) $sandbox->eval('require("m0")', '=depth');
	echo "depth: NOT ENFORCED\n";
} catch (Throwable $error) {
	printf("depth: %s (fatal: %s)\n", $error->getMessage(),
		var_export($error instanceof FatalError, true));
}

$sandbox->close();

// The module count is a separate bound: these are siblings, never nested.
$flat = [];

for ($index = 0; $index < 8; $index++) {
	$flat[sprintf('/f%d.lua', $index)] = 'return {}';
}

$sandbox = $make($flat, new Limits(maxModules: 3));

try {
	(void) $sandbox->eval(
		'for index = 0, 7 do require("f" .. index) end',
		'=count',
	);
	echo "count: NOT ENFORCED\n";
} catch (Throwable $error) {
	printf("count: %s\n", $error->getMessage());
}

$sandbox->close();

// A module that failed while loading is NOT cached, so a later require gets a
// fresh attempt rather than replaying the failure. Upstream leaves the sentinel
// behind and cannot offer this.
$sandbox = $make(['/flaky.lua' => 'if not RETRY then error("first attempt fails") end return { ok = true }']);

$first = $sandbox->eval('local ok, err = pcall(require, "flaky") return ok, tostring(err)', '=retry');
printf("first:  ok=%s\n", var_export($first[0], true));

$second = $sandbox->eval('RETRY = true local m = require("flaky") return m.ok', '=retry');
printf("second: ok=%s\n", var_export($second[0], true));

$sandbox->close();

// A module that does not COMPILE is a different path from one that fails while
// running: the raise comes from inside the loader, before any chunk exists.
// It is here so the valgrind leg walks it -- that raise is a longjmp, and the
// source and chunk name held across it leaked until they were moved into
// Lua-owned memory. An RSS assertion in a .phpt would be flaky on a shared
// runner, so the sanitizer legs are the regression guard rather than this file.
$sandbox = $make(['/broken.lua' => 'this is not ( valid lua']);

try {
	(void) $sandbox->eval('require("broken")', '=broken');
	echo "compile: NOT REPORTED\n";
} catch (Throwable $error) {
	printf("compile: %s\n", str_contains($error->getMessage(), 'broken') ? 'names the module' : 'unhelpful');
}

$sandbox->close();

// Names are validated before the VFS or a resolver sees them, because a name
// becomes part of a path.
$sandbox = $make([]);

foreach (['../etc/passwd', 'a/b', "nul\0byte", str_repeat('x', 200)] as $name) {
	try {
		(void) $sandbox->eval(sprintf('require(%s)', var_export($name, true)), '=name');
		printf("name %-16s ACCEPTED\n", substr(str_replace("\0", '\0', $name), 0, 16));
	} catch (Throwable $error) {
		printf("name %-16s refused\n", substr(str_replace("\0", '\0', $name), 0, 16));
	}
}

$sandbox->close();

?>
--EXPECTF--
cycle: Module "a" requires itself, directly or through another module
depth: Loading "m%d" would nest require() 5 deep, which is the sandbox's Limits::$maxRequireDepth (fatal: true)
count: The sandbox has already loaded 3 module(s), which is its Limits::$maxModules
first:  ok=false
second: ok=true
compile: names the module
name ../etc/passwd    refused
name a/b              refused
name nul\0byte        refused
name xxxxxxxxxxxxxxxx refused
