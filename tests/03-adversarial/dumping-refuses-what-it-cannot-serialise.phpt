--TEST--
dump() refuses a host callable rather than reading a Proto out of a C closure
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Capabilities;
use DevelopGravity\LuaExt\Exception\CapabilityError;
use DevelopGravity\LuaExt\Exception\ClosedSandboxError;
use DevelopGravity\LuaExt\Exception\RuntimeError;
use DevelopGravity\LuaExt\Sandbox;
use DevelopGravity\LuaExt\SandboxConfig;

// THE CASE THIS FILE EXISTS FOR is the first one.
//
// Lua 5.5's lua_dump guards "is this a Lua function" with api_check(), which
// compiles to NOTHING unless LUA_USE_APICHECK is defined -- and then reads
// clLvalue(f)->p unconditionally. wrapCallable() produces a C closure, so an
// unguarded dump() reads a Proto pointer out of a CClosure.
//
// That is not a hypothetical. With the guard removed, this exact call reads a
// garbage length and asks the allocator for 11.3 GB. And because this build
// defines LUA_USE_APICHECK only under --enable-luaext-debug, the unguarded
// version asserts cleanly in a debug build and corrupts memory in a release
// one -- the worst possible split.

$capabilities = (new Capabilities())->with(dumpBytecode: true);
$sandbox = new Sandbox(new SandboxConfig(capabilities: $capabilities));

$wrapped = $sandbox->wrapCallable(static fn (int $value): int => $value + 1, 'hostFn');

try {
	$wrapped->dump();
	echo "host callable: DUMPED\n";
} catch (RuntimeError $error) {
	printf("host callable: %s\n", $error->getMessage());
}

// The handle itself is unharmed -- refusing is not the same as invalidating.
printf("still callable: %d\n", $wrapped->call(41)[0]);
printf("still valid:    %s\n", var_export($wrapped->isValid(), true));

$sandbox->close();

// Producing bytecode needs its own capability, and the untrusted default is off.
$plain = new Sandbox();

try {
	$plain->compile('return 1')->dump();
	echo "without capability: DUMPED\n";
} catch (CapabilityError $error) {
	printf("without capability: %s\n", $error->getMessage());
}

$plain->close();

// A handle outliving its sandbox reports that, rather than reaching into a
// registry that no longer exists.
$closing = new Sandbox(new SandboxConfig(capabilities: $capabilities));
$orphan = $closing->compile('return 1');
$closing->close();

try {
	$orphan->dump();
	echo "after close: DUMPED\n";
} catch (ClosedSandboxError $error) {
	printf("after close: %s\n", $error->getMessage());
}

?>
--EXPECT--
host callable: This LuaFunction wraps a PHP callable, which has no bytecode to dump. Only a function compiled from Lua source can be dumped
still callable: 42
still valid:    true
without capability: Dumping a function to bytecode requires the dumpBytecode capability, which this sandbox was not granted
after close: The sandbox has been closed
