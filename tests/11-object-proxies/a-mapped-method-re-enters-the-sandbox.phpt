--TEST--
A mapped method driving the sandbox from inside its own dispatch keeps its receiver
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\LuaMethod;
use DevelopGravity\LuaExt\LuaOperator;
use DevelopGravity\LuaExt\Operator;
use DevelopGravity\LuaExt\Sandbox;

final class Recurse
{
	public static ?Sandbox $sandbox = null;

	public static int $destroyed = 0;

	#[LuaMethod]
	public function __construct(public readonly int $value = 0) {}

	#[LuaMethod]
	public function value(): int
	{
		return $this->value;
	}

	/**
	 * Runs further Lua on the very sandbox that is mid-dispatch into this
	 * method. The receiver's proxy is anchored on the caller's stack for the
	 * duration; a nested run that collected it would pull the zend_object out
	 * from under $this.
	 */
	#[LuaMethod]
	public function doubled(): int
	{
		$inner = self::$sandbox->eval('collectgarbage("collect") return 2')[0];

		return $this->value * $inner;
	}

	#[LuaOperator(Operator::Add)]
	public function plus(self $other): int
	{
		// The same, from an operator handler, where BOTH operands are anchored.
		(void) self::$sandbox->eval('collectgarbage("collect")');

		return $this->value + $other->value;
	}

	public function __destruct()
	{
		self::$destroyed++;
	}
}

$sandbox = new Sandbox();
Recurse::$sandbox = $sandbox;
$sandbox->registerClass(Recurse::class, luaName: 'r');

// A method that re-enters and forces a full collection mid-dispatch.
var_dump($sandbox->eval('local a = r.new(21) return a:doubled()')[0]);

// The receiver survives the nested run and is still usable afterwards.
var_dump($sandbox->eval(<<<'LUA'
	local a = r.new(5)
	local first = a:doubled()
	local second = a:value()
	return first, second
LUA));

// An operator handler doing the same, with two live operands on the stack.
var_dump($sandbox->eval('return r.new(3) + r.new(4)')[0]);

// Nested dispatch nests: the inner run calls the method again.
$sandbox->registerLibrary('host', ['bounce' => static function (): int {
	return self_bounce();
}]);

function self_bounce(): int
{
	return Recurse::$sandbox->eval('return r.new(6):value()')[0];
}

var_dump($sandbox->eval('return host.bounce()')[0]);

(void) $sandbox->eval('collectgarbage("collect")');
var_dump($sandbox->stats()->liveObjectProxies);

$sandbox->close();
Recurse::$sandbox = null;

?>
--EXPECT--
int(42)
array(2) {
  [0]=>
  int(10)
  [1]=>
  int(5)
}
int(7)
int(6)
int(0)
