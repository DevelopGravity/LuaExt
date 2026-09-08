--TEST--
Every mappable operator dispatches: the arithmetic family, unary minus, and concatenation
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Exception\RuntimeError;
use DevelopGravity\LuaExt\LuaMethod;
use DevelopGravity\LuaExt\LuaOperator;
use DevelopGravity\LuaExt\Operator;
use DevelopGravity\LuaExt\Sandbox;

final class Num
{
	#[LuaMethod]
	public function __construct(public readonly int $value = 0) {}

	#[LuaMethod]
	public function get(): int { return $this->value; }

	#[LuaOperator(Operator::Subtract)]
	public function minus(self $other): self { return new self($this->value - $other->value); }

	#[LuaOperator(Operator::Multiply)]
	public function times(self $other): self { return new self($this->value * $other->value); }

	#[LuaOperator(Operator::Divide)]
	public function dividedBy(self $other): self { return new self(intdiv($this->value, $other->value)); }

	#[LuaOperator(Operator::Modulo)]
	public function modulo(self $other): self { return new self($this->value % $other->value); }

	#[LuaOperator(Operator::Power)]
	public function toThe(self $other): self { return new self((int)($this->value ** $other->value)); }

	#[LuaOperator(Operator::UnaryMinus)]
	public function negated(): self { return new self(-$this->value); }

	#[LuaOperator(Operator::Concatenate)]
	public function joined(self $other): string { return "{$this->value}|{$other->value}"; }
}

// UnaryMinus dispatches through its own structurally distinct path
// (luaext_proxy_unop): single operand, its own upvalue layout. A thrown
// RuntimeError must surface there exactly as it does for the binary family.
final class Sour
{
	#[LuaOperator(Operator::UnaryMinus)]
	public function refuse(): never { throw new RuntimeError('unary soft'); }
}

$sandbox = new Sandbox();
$sandbox->registerClass(Num::class);
$sandbox->registerClass(Sour::class);

(void) $sandbox->eval('a = Num.new(6) b = Num.new(3)');

var_dump($sandbox->eval(<<<'LUA'
	return (a - b):get(), (a * b):get(), (a / b):get(),
		   (a % b):get(), (a ^ b):get(), (-a):get(), a .. b
LUA));

// The unary result is a live proxy, so it chains like any other.
var_dump($sandbox->eval('return (-(-a)):get()')[0]);

$sandbox->setGlobal('s', new Sour());
var_dump($sandbox->eval('local ok, err = pcall(function() return -s end) return ok, tostring(err)'));

$sandbox->close();

?>
--EXPECT--
array(7) {
  [0]=>
  int(3)
  [1]=>
  int(18)
  [2]=>
  int(2)
  [3]=>
  int(0)
  [4]=>
  int(216)
  [5]=>
  int(-6)
  [6]=>
  string(3) "6|3"
}
int(6)
array(2) {
  [0]=>
  bool(false)
  [1]=>
  string(10) "unary soft"
}
