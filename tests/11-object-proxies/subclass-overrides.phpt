--TEST--
A subclass instance wrapping as its registered ancestor still runs its own overrides
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\LuaMethod;
use DevelopGravity\LuaExt\LuaOperator;
use DevelopGravity\LuaExt\Operator;
use DevelopGravity\LuaExt\Sandbox;

class Duration
{
	#[LuaMethod]
	public function __construct(protected int $seconds = 0) {}

	#[LuaMethod]
	public function seconds(): int
	{
		return $this->seconds;
	}

	#[LuaMethod]
	public function __toString(): string
	{
		return "base:{$this->seconds}";
	}

	#[LuaOperator(Operator::Add)]
	public function plus(self $other): static
	{
		return new static($this->seconds + $other->seconds);
	}
}

final class PreciseDuration extends Duration
{
	public function seconds(): int
	{
		return $this->seconds * 1000;
	}

	public function __toString(): string
	{
		return "precise:{$this->seconds}";
	}

	public function plus(Duration $other): static
	{
		// Doubled, so the override is unmistakable in the result.
		return new static(($this->seconds + $other->seconds) * 2);
	}
}

$sandbox = new Sandbox();
$sandbox->registerClass(Duration::class);

// Wraps as Duration (the nearest registered ancestor) yet behaves as itself:
// PHP polymorphism holds through the proxy for methods, tostring, and mapped
// operators alike.
$sandbox->setGlobal('p', new PreciseDuration(2));
$sandbox->setGlobal('b', new Duration(2));

var_dump($sandbox->eval('return p:seconds(), b:seconds()'));
var_dump($sandbox->eval('return tostring(p), tostring(b)'));

// The overridden operator runs, and its `new static` result wraps and keeps
// behaving as the subclass.
var_dump($sandbox->eval('return (p + b):seconds()')[0]);
var_dump($sandbox->eval('return (b + b):seconds()')[0]);

$sandbox->close();

?>
--EXPECT--
array(2) {
  [0]=>
  int(2000)
  [1]=>
  int(2)
}
array(2) {
  [0]=>
  string(9) "precise:2"
  [1]=>
  string(6) "base:2"
}
int(8000)
int(4)
