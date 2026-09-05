--TEST--
Touching a withheld feature raises FeatureNotGrantedError naming the capability
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Capabilities;
use DevelopGravity\LuaExt\Exception\FeatureNotGrantedError;
use DevelopGravity\LuaExt\Exception\RuntimeError;
use DevelopGravity\LuaExt\Sandbox;
use DevelopGravity\LuaExt\SandboxConfig;

// Two tiers, split by whether the module exists at all.
//
// Tier 1 -- libraries that go WHOLLY absent (coroutine, utf8, require, debug):
// the global stays genuinely nil, so `if coroutine then` keeps taking the
// absent branch, and the patched error path classifies the touch instead of
// reporting a bare "attempt to index a nil value".
//
// Tier 2 -- gated members of modules that always exist (os, io, string, debug
// with some capability, and the base library): the slot holds a gate stub, a
// truthy function whose only behaviour is to raise.
//
// Native Lua is the truth source for everything the classification does NOT
// touch: unrelated nils keep upstream's exact message and the plain
// RuntimeError class.

$bare = new Sandbox(new SandboxConfig(
	capabilities: (new Capabilities())->with(coroutines: false, utf8: false, debugTraceback: false),
));

// Tier 1, the index shape and the call shape.
foreach ([
	'coroutine index' => 'return coroutine.create(function() end)',
	'coroutine arith' => 'return coroutine + 1',
	'utf8 index     ' => 'return utf8.len("x")',
	'require call   ' => 'return require("m")',
	'debug index    ' => 'return debug.traceback()',
] as $label => $script) {
	try {
		(void) $bare->eval($script, '=tier1');
		printf("%s => RAN\n", $label);
	} catch (FeatureNotGrantedError $error) {
		printf("%s => %s (line %s)\n", $label, $error->getMessage(), var_export($error->getLuaLine(), true));
	}
}

// The blessed probe: short-circuit keeps the nil untouched, both branches.
var_dump($bare->eval(
	'return (type(coroutine) == "table" and type(coroutine.create) == "function")', '=probe')[0]);

// A script's own values are never accused: assigning to a withheld name works,
// and a wrong-typed own value keeps upstream's wrong-type message.
var_dump($bare->eval('coroutine = {} return type(coroutine)', '=shadow')[0]);

try {
	(void) $bare->eval('local t = nil return t.x', '=plain-nil');
} catch (RuntimeError $error) {
	printf("unrelated nil    => %s: %s\n", substr(strrchr($error::class, '\\'), 1), $error->getMessage());
}

$bare->close();

// Tier 2: every gate raises with its member and capability named. The
// untrusted default withholds all of these.
$untrusted = new Sandbox(new SandboxConfig());

foreach ([
	'return os.getenv("PATH")',
	'return io.open("/f", "r")',
	'return io.lines("/f")',
	'return string.dump(print)',
	'return load("return 1")',
	'return warn("x")',
	'return os.remove("/f")',
] as $script) {
	try {
		(void) $untrusted->eval($script, '=tier2');
		printf("RAN: %s\n", $script);
	} catch (FeatureNotGrantedError $error) {
		printf("%s\n", $error->getMessage());
	}
}

// Granted, the same touches run (or argue about their arguments, which is the
// member's own code running -- the point).
$granted = new Sandbox(new SandboxConfig(
	capabilities: Capabilities::untrusted()->with(compileAtRuntime: true, warn: true),
));

var_dump($granted->eval('return type(load("return 1"))', '=granted-load')[0]);
var_dump($granted->eval('return coroutine.wrap(function() coroutine.yield(7) end)()', '=granted-coro')[0]);

// And in a granted sandbox a script that niles out its own coroutine gets the
// stock upstream message -- the name is not withheld here, so the classifier
// stays out of it.
try {
	(void) $granted->eval('coroutine = nil return coroutine.create', '=granted-shadow');
} catch (RuntimeError $error) {
	printf("granted shadow   => %s: %s\n", substr(strrchr($error::class, '\\'), 1), $error->getMessage());
}

$granted->close();

?>
--EXPECT--
coroutine index => The script used coroutine, which needs the coroutines capability this sandbox was not granted (line 1)
coroutine arith => The script used coroutine, which needs the coroutines capability this sandbox was not granted (line 1)
utf8 index      => The script used utf8, which needs the utf8 capability this sandbox was not granted (line 1)
require call    => The script used require, which needs the require capability this sandbox was not granted (line 1)
debug index     => The script used debug, which needs the debugTraceback capability this sandbox was not granted (line 1)
bool(false)
string(5) "table"
unrelated nil    => RuntimeError: plain-nil:1: attempt to index a nil value (local 't')
The script called os.getenv, which needs the osEnv capability this sandbox was not granted
The script called io.open, which needs the vfs capability this sandbox was not granted
The script called io.lines, which needs the vfs capability this sandbox was not granted
The script called string.dump, which needs the dumpBytecode capability this sandbox was not granted
The script called load, which needs the compileAtRuntime capability this sandbox was not granted
The script called warn, which needs the warn capability this sandbox was not granted
The script called os.remove, which needs the vfs capability this sandbox was not granted
string(8) "function"
int(7)
granted shadow   => RuntimeError: granted-shadow:1: attempt to index a nil value (global 'coroutine')
