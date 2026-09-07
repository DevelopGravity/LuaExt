--TEST--
Dot-calls, wrong receivers, and thrown exceptions follow the boundary rules
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Exception\RuntimeError;
use DevelopGravity\LuaExt\LuaMethod;
use DevelopGravity\LuaExt\Sandbox;

final class Fussy
{
	#[LuaMethod]
	public function poke(): string
	{
		return 'poked';
	}

	#[LuaMethod]
	public function soft(): never
	{
		throw new RuntimeError('soft failure');
	}

	#[LuaMethod]
	public function hard(): never
	{
		throw new LogicException('hard failure');
	}
}

final class Other
{
	#[LuaMethod]
	public function poke(): string
	{
		return 'other';
	}
}

$sandbox = new Sandbox();
$sandbox->registerClass(Fussy::class);
$sandbox->registerClass(Other::class);
$sandbox->setGlobal('f', new Fussy());
$sandbox->setGlobal('o', new Other());

// A dot-call has no receiver: catchable, and the message names the fix.
var_dump($sandbox->eval('local ok, err = pcall(function() return f.poke() end) return ok, tostring(err)'));

// A receiver of the wrong class is the same mistake.
var_dump($sandbox->eval('local ok, err = pcall(function() return f.poke(o) end) return ok, tostring(err)'));

// RuntimeError is the script's to catch; anything else aborts.
var_dump($sandbox->eval('local ok, err = pcall(function() return f:soft() end) return ok, tostring(err)'));

try {
	(void) $sandbox->eval('pcall(function() return f:hard() end)');
} catch (LogicException $error) {
	echo 'aborted: ', $error->getMessage(), "\n";
}

$sandbox->close();

?>
--EXPECT--
array(2) {
  [0]=>
  bool(false)
  [1]=>
  string(57) "method 'poke' must be called with a colon (obj:poke(...))"
}
array(2) {
  [0]=>
  bool(false)
  [1]=>
  string(57) "method 'poke' must be called with a colon (obj:poke(...))"
}
array(2) {
  [0]=>
  bool(false)
  [1]=>
  string(12) "soft failure"
}
aborted: hard failure
