--TEST--
Float table keys are refused; integral ones were never floats to begin with
--EXTENSIONS--
luaext
--XFAIL--
Needs Sandbox::eval(), which lands with the execution subsystem.
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Exception\ConversionError;
use DevelopGravity\LuaExt\Sandbox;

$sandbox = new Sandbox();

// Lua normalises a float key whose value is an exact integer to an integer
// key, so t[1.0] and t[1] are one key and there is nothing to distinguish.
[$normalised] = $sandbox->eval('local t = {}; t[1.0] = "written as a float"; return t');
var_dump($normalised, array_map('gettype', array_keys($normalised)));
var_dump($sandbox->eval('local t = {}; t[1] = "a"; t[1.0] = "b"; local n = 0; for _ in pairs(t) do n = n + 1 end; return n, t[1]'));

// What is left over is genuinely fractional, or too large for an integer.
// PHP has no float keys: it would truncate 1.5 onto the integer key 1 and
// merge it with whatever is already there, so this is refused instead.
foreach (['return { [1.5] = "fraction" }', 'return { [2.0 ^ 70] = "huge" }'] as $chunk) {
	try {
		printf("NOT REFUSED: %s\n", var_export($sandbox->eval($chunk), true));
	} catch (ConversionError $error) {
		printf("%s float=%s\n",
			$error::class,
			var_export(str_contains($error->getMessage(), 'float'), true));
	}
}

?>
--EXPECT--
array(1) {
  [1]=>
  string(18) "written as a float"
}
array(1) {
  [0]=>
  string(7) "integer"
}
array(2) {
  [0]=>
  int(1)
  [1]=>
  string(1) "b"
}
DevelopGravity\LuaExt\Exception\ConversionError float=true
DevelopGravity\LuaExt\Exception\ConversionError float=true
