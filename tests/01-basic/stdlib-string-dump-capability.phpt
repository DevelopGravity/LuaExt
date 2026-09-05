--TEST--
string.dump needs dumpBytecode, through the global and the string metatable alike
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Capabilities;
use DevelopGravity\LuaExt\Exception\FeatureNotGrantedError;
use DevelopGravity\LuaExt\Sandbox;
use DevelopGravity\LuaExt\SandboxConfig;

// string.dump serialises a function to bytecode, and Lua has no bytecode
// verifier: a sandbox that can produce bytecode and one that can load it are a
// single capability apart from arbitrary native execution.
//
// The second half of this test is the part that is easy to get wrong.
// luaopen_string points getmetatable("").__index at its OWN table, so every
// method call on a string literal goes through a complete second copy of the
// library. Gating only the global would leave ("x"):dump() working through a
// path no walk of _G would ever find. Withheld, the slot holds a gate stub --
// truthy, so type() is not a probe here -- and because the metatable indexes
// the same table, BOTH routes reach the same gate.

$probe = <<<'LUA'
	local through_global = string.dump
	local through_metatable = ("x").dump
	local index = getmetatable("").__index

	return type(through_global), type(through_metatable),
		through_global == through_metatable, index == string
LUA;

$withheld = new Sandbox(new SandboxConfig());
var_dump($withheld->eval($probe));

// The gate raises through either route, naming the capability.
foreach (['global' => 'string.dump(print)', 'method' => '("x"):dump()'] as $route => $call) {
	try {
		(void) $withheld->eval('return ' . $call, '=dump-gate');
		printf("%s route: DUMPED WITHOUT THE CAPABILITY\n", $route);
	} catch (FeatureNotGrantedError $error) {
		printf("%s route: %s\n", $route, $error->getMessage());
	}
}

$withheld->close();

$granted = new Sandbox(new SandboxConfig(
	capabilities: (new Capabilities())->with(dumpBytecode: true),
));
var_dump($granted->eval($probe));

// Granted, it is reachable by both routes, they are the same function, and it
// actually dumps.
var_dump($granted->eval('return type(string.dump(function() return 1 end))'));

$granted->close();

?>
--EXPECT--
array(4) {
  [0]=>
  string(8) "function"
  [1]=>
  string(8) "function"
  [2]=>
  bool(true)
  [3]=>
  bool(true)
}
global route: The script called string.dump, which needs the dumpBytecode capability this sandbox was not granted
method route: The script called string.dump, which needs the dumpBytecode capability this sandbox was not granted
array(4) {
  [0]=>
  string(8) "function"
  [1]=>
  string(8) "function"
  [2]=>
  bool(true)
  [3]=>
  bool(true)
}
array(1) {
  [0]=>
  string(6) "string"
}
