--TEST--
string.dump needs dumpBytecode, and the string metatable is filtered too
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Capabilities;
use DevelopGravity\LuaExt\Sandbox;
use DevelopGravity\LuaExt\SandboxConfig;

// string.dump serialises a function to bytecode, and Lua has no bytecode
// verifier: a sandbox that can produce bytecode and one that can load it are a
// single capability apart from arbitrary native execution.
//
// The second half of this test is the part that is easy to get wrong.
// luaopen_string points getmetatable("").__index at its OWN table, so every
// method call on a string literal goes through a complete second copy of the
// library. Filtering only the global would withhold string.dump and leave
// ("x"):dump() working through a path no walk of _G would ever find.

$probe = <<<'LUA'
	local through_global = string.dump
	local through_metatable = ("x").dump
	local index = getmetatable("").__index

	return type(through_global), type(through_metatable), index == string
LUA;

$sandbox = new Sandbox();
var_dump($sandbox->eval($probe));
$sandbox->close();

$sandbox = new Sandbox(new SandboxConfig(
	capabilities: (new Capabilities())->with(dumpBytecode: true),
));
var_dump($sandbox->eval($probe));

// Granted, it is reachable by both routes and they are the same function.
var_dump($sandbox->eval('return ("x").dump == string.dump, type(string.dump(function() return 1 end))'));

$sandbox->close();

?>
--EXPECT--
array(3) {
  [0]=>
  string(3) "nil"
  [1]=>
  string(3) "nil"
  [2]=>
  bool(true)
}
array(3) {
  [0]=>
  string(8) "function"
  [1]=>
  string(8) "function"
  [2]=>
  bool(true)
}
array(2) {
  [0]=>
  bool(true)
  [1]=>
  string(6) "string"
}
