--TEST--
PHP integer keys use the full int64 range, in both directions
--EXTENSIONS--
luaext
--XFAIL--
Needs Sandbox::eval()/setGlobal(), which land with the execution subsystem.
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Sandbox;

// Lua 5.1 had only doubles, so the extension this replaces had to stringify
// any key past 2**53 and the key silently changed type on the way through.
// Lua has real 64-bit integers now, so every PHP integer key is pushed as an
// integer and arrives as itself.
$keys = [
	'math.maxinteger' => PHP_INT_MAX,
	'math.mininteger' => PHP_INT_MIN,
	'1 << 53' => 9007199254740992,
	'(1 << 53) + 1' => 9007199254740993,
	'-(1 << 53) - 1' => -9007199254740993,
	'0' => 0,
	'-1' => -1,
];

$array = [];

foreach ($keys as $value) {
	$array[$value] = "at $value";
}

$sandbox = new Sandbox();
$sandbox->setGlobal('t', $array);

// Read every key back with the Lua expression that names it, so the assertion
// goes through Lua's own integer parsing rather than PHP's.
foreach ($keys as $expression => $value) {
	[$found] = $sandbox->eval("return t[$expression]");
	printf("%-16s %s\n", $expression, var_export($found === "at $value", true));
}

// 2**53 + 1 is the first integer a double cannot represent: if either side
// ever routes a key through a float, these two collapse onto one key.
var_dump(count($array), $sandbox->eval('return t[1 << 53] ~= t[(1 << 53) + 1]'));

// And the same boundaries coming the other way.
[$back] = $sandbox->eval(<<<'LUA'
	return {
		[math.maxinteger] = "max",
		[math.mininteger] = "min",
		[(1 << 53) + 1] = "beyond double",
	}
	LUA);

var_dump($back[PHP_INT_MAX], $back[PHP_INT_MIN], $back[9007199254740993]);
var_dump(array_map('gettype', array_keys($back)));

?>
--EXPECT--
math.maxinteger  true
math.mininteger  true
1 << 53          true
(1 << 53) + 1    true
-(1 << 53) - 1   true
0                true
-1               true
int(7)
array(1) {
  [0]=>
  bool(true)
}
string(3) "max"
string(3) "min"
string(13) "beyond double"
array(3) {
  [0]=>
  string(7) "integer"
  [1]=>
  string(7) "integer"
  [2]=>
  string(7) "integer"
}
