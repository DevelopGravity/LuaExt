--TEST--
Proxy equality follows the chain: pointer, ours-only, same-class, never abort
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\LuaMethod;
use DevelopGravity\LuaExt\Sandbox;

final class Left
{
	#[LuaMethod]
	public function id(): int { return 1; }
}

final class Right
{
	#[LuaMethod]
	public function id(): int { return 2; }
}

$sandbox = new Sandbox();
$sandbox->registerClass(Left::class);
$sandbox->registerClass(Right::class);

$one = new Left();
$sandbox->setGlobal('a', $one);
$sandbox->setGlobal('b', $one);          // same object, second crossing
$sandbox->setGlobal('c', new Left());    // different object, same class
$sandbox->setGlobal('d', new Right());   // different registered class

// Plain assignment is Lua's own reference copy: identical value, same table key.
var_dump($sandbox->eval('local e = a local t = {} t[a] = "x" return a == e, t[e]'));

// Two crossings of one object: __eq true, but distinct table keys.
var_dump($sandbox->eval('local t = {} t[a] = "x" return a == b, t[b] == nil'));

// Distinct objects and cross-class pairs are false, never an error.
var_dump($sandbox->eval('return a == c, a == d, d == a'));

$sandbox->close();

?>
--EXPECT--
array(2) {
  [0]=>
  bool(true)
  [1]=>
  string(1) "x"
}
array(2) {
  [0]=>
  bool(true)
  [1]=>
  bool(true)
}
array(3) {
  [0]=>
  bool(false)
  [1]=>
  bool(false)
  [2]=>
  bool(false)
}
