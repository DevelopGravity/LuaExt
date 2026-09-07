--TEST--
Mapped operators dispatch; unmapped and mixed shapes fail catchably; equality maps by value
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\LuaMethod;
use DevelopGravity\LuaExt\LuaOperator;
use DevelopGravity\LuaExt\Operator;
use DevelopGravity\LuaExt\Sandbox;

final class Span
{
	#[LuaMethod]
	public function __construct(private int $length = 0) {}

	#[LuaMethod]
	public function length(): int { return $this->length; }

	#[LuaOperator(Operator::LessThan)]
	public function shorterThan(self $other): bool
	{
		return $this->length < $other->length;
	}

	#[LuaOperator(Operator::LessThanOrEqual)]
	public function noLongerThan(self $other): bool
	{
		return $this->length <= $other->length;
	}

	#[LuaOperator(Operator::Equality)]
	public function sameLength(self $other): bool
	{
		return $this->length === $other->length;
	}

	#[LuaOperator(Operator::Add)]
	public function plus(self $other): self
	{
		return new self($this->length + $other->length);
	}
}

$sandbox = new Sandbox();
$sandbox->registerClass(Span::class);

(void) $sandbox->eval('a = Span.new(2) b = Span.new(5) c = Span.new(2)');

// Comparison, with > and >= derived by operand swap.
var_dump($sandbox->eval('return a < b, b < a, a <= c, b > a, a >= c'));

// Arithmetic chains: the sum is a live proxy.
var_dump($sandbox->eval('return (a + b):length()')[0]);

// Mapped equality is by value; pointer fast path still holds.
var_dump($sandbox->eval('return a == c, a == b, a == a'));

// A mapped method is not thereby callable by name.
var_dump($sandbox->eval('return a.plus == nil')[0]);

// Mixed shapes are catchable, not fatal; subtraction was never mapped.
var_dump($sandbox->eval('local ok = pcall(function() return a + 1 end) return ok')[0]);
var_dump($sandbox->eval('local ok = pcall(function() return a - b end) return ok')[0]);

$sandbox->close();

?>
--EXPECT--
array(5) {
  [0]=>
  bool(true)
  [1]=>
  bool(false)
  [2]=>
  bool(true)
  [3]=>
  bool(true)
  [4]=>
  bool(true)
}
int(7)
array(3) {
  [0]=>
  bool(true)
  [1]=>
  bool(false)
  [2]=>
  bool(true)
}
bool(true)
bool(false)
bool(false)
