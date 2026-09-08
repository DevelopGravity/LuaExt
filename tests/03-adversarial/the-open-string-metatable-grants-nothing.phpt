--TEST--
The writable string metatable is stock Lua, and writing to it recovers nothing withheld
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Sandbox;

// getmetatable("") answering a writable metatable is documented Lua, so the
// sandbox leaves it open -- the one metatable it filters but does not lock.
// That is safe only as long as writing there can never reach a withheld
// member: the metatable's __index is the filtered surface (where dump is a
// stub that raises its capability, fatally), the unfiltered library table the
// filter displaced is unreachable, and the worst a script can do is break
// string methods in its own state. This pins exactly that bargain.
$sandbox = new Sandbox();

// Stock behaviour: the metatable is reachable and __index is a table.
$results = $sandbox->eval(
	'local mt = getmetatable("") return type(mt), type(mt.__index)', '=stock-shape');
var_dump($results[0], $results[1]);

// The dump reached through the metatable is the refusing stub, not the real
// one: calling it raises the capability fatally -- past pcall, out to the
// host -- instead of producing bytecode.
try {
	(void) $sandbox->eval(
		'return select(2, pcall(getmetatable("").__index.dump, function() end))',
		'=metatable-route');
} catch (Throwable $error) {
	printf("metatable route: %s\n", $error->getMessage());
}

// Sabotage is permitted and stays local: replacing __index only loses
// methods for this state, it resurrects nothing.
$results = $sandbox->eval(<<<'LUA'
	getmetatable("").__index = {}
	return tostring(("x").dump), tostring(("x").rep)
LUA, '=sabotage');
var_dump($results[0], $results[1]);

$sandbox->close();

?>
--EXPECT--
string(5) "table"
string(5) "table"
metatable route: The script called string.dump, which needs the dumpBytecode capability this sandbox was not granted
string(3) "nil"
string(3) "nil"
