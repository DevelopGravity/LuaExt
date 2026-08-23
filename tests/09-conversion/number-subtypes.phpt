--TEST--
Lua integers and floats stay distinct in both directions
--EXTENSIONS--
luaext
--XFAIL--
Needs Sandbox::eval()/setGlobal(), which land with the execution subsystem.
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Sandbox;

$sandbox = new Sandbox();

// Lua has had a real integer type since 5.3, so 2 and 2.0 are different
// values. Deciding by lua_isinteger rather than by looking at the value is
// what keeps them different on the PHP side too.
var_dump($sandbox->eval('return 2, 2.0'));

// The arithmetic that produces each subtype, so the mapping is checked against
// values Lua computed rather than values a literal declared.
var_dump($sandbox->eval('return 7 // 2, 7 / 2, 2^2, math.floor(2.5)'));

// PHP -> Lua keeps the same distinction; math.type is Lua's own answer.
$sandbox->setGlobal('integer', 2);
$sandbox->setGlobal('float', 2.0);
var_dump($sandbox->eval('return math.type(integer), math.type(float)'));

// Non-finite floats have no integer subtype to be confused with, and survive.
$sandbox->setGlobal('infinite', INF);
var_dump($sandbox->getGlobal('infinite'), $sandbox->eval('return math.type(infinite)'));
var_dump(is_nan($sandbox->eval('return 0/0')[0]));

?>
--EXPECT--
array(2) {
  [0]=>
  int(2)
  [1]=>
  float(2)
}
array(4) {
  [0]=>
  int(3)
  [1]=>
  float(3.5)
  [2]=>
  float(4)
  [3]=>
  int(2)
}
array(2) {
  [0]=>
  string(7) "integer"
  [1]=>
  string(5) "float"
}
float(INF)
array(1) {
  [0]=>
  string(5) "float"
}
bool(true)
