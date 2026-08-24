--TEST--
A Lua function becomes a LuaFunction, and only its own sandbox accepts it back
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Exception\ConversionError;
use DevelopGravity\LuaExt\LuaFunction;
use DevelopGravity\LuaExt\Sandbox;

$sandbox = new Sandbox();

[$adder] = $sandbox->eval('return function(a, b) return a + b, a * b end');

var_dump($adder instanceof LuaFunction, $adder->isValid(), $adder->getSandbox() === $sandbox);
var_dump($adder->call(2, 3), $adder(4, 5));

// Round trip: a handle pushed back into the sandbox it came from is the same
// function, not a copy of it.
$sandbox->setGlobal('adder', $adder);
var_dump($sandbox->eval('return adder(10, 5)'));
var_dump($sandbox->eval('return adder == adder'));

// Functions nested inside a table become handles too.
[$table] = $sandbox->eval('return { double = function(n) return n * 2 end }');
var_dump($table['double'] instanceof LuaFunction, $table['double']->call(21));

// A handle names a slot in one sandbox's registry. Pushing it into another
// interpreter would read an unrelated slot, so it is refused rather than
// silently reinterpreted.
$other = new Sandbox();

try {
	$other->setGlobal('foreign', $adder);
	echo "NOT REFUSED\n";
} catch (ConversionError $error) {
	printf("%s different sandbox=%s\n",
		$error::class,
		var_export(str_contains($error->getMessage(), 'different sandbox'), true));
}

$other->close();

// The refusal did not disturb the sandbox that does own the handle.
var_dump($sandbox->eval('return adder(1, 1)'));

?>
--EXPECT--
bool(true)
bool(true)
bool(true)
array(2) {
  [0]=>
  int(5)
  [1]=>
  int(6)
}
array(2) {
  [0]=>
  int(9)
  [1]=>
  int(20)
}
array(2) {
  [0]=>
  int(15)
  [1]=>
  int(50)
}
array(1) {
  [0]=>
  bool(true)
}
bool(true)
array(1) {
  [0]=>
  int(42)
}
DevelopGravity\LuaExt\Exception\ConversionError different sandbox=true
array(2) {
  [0]=>
  int(2)
  [1]=>
  int(1)
}
