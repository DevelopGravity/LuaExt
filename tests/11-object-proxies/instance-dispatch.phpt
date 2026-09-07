--TEST--
Proxy methods dispatch with the colon convention, chain, and honour name overrides
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\LuaMethod;
use DevelopGravity\LuaExt\Sandbox;

final class Counter
{
	private int $value = 0;

	#[LuaMethod]
	public function add(int $amount): static
	{
		$this->value += $amount;
		return $this;
	}

	#[LuaMethod]
	public function fork(): static
	{
		return clone $this;
	}

	#[LuaMethod('total')]
	public function currentValue(): int
	{
		return $this->value;
	}

	#[LuaMethod]
	public function __toString(): string
	{
		return "counter@{$this->value}";
	}

	public function hidden(): int { return -1; }
}

$sandbox = new Sandbox();
$sandbox->registerClass(Counter::class);
$sandbox->setGlobal('c', new Counter());

// Chaining through $this, a cloned instance, and the name override.
var_dump($sandbox->eval('return c:add(2):add(3):total()')[0]);
var_dump($sandbox->eval('local f = c:fork():add(10) return f:total(), c:total()'));

// tostring() routes through the marked __toString.
var_dump($sandbox->eval('return tostring(c)')[0]);

// Unmarked methods read as nil; unknown names fail the standard Lua way.
var_dump($sandbox->eval('return c.hidden == nil')[0]);
var_dump($sandbox->eval('local ok, err = pcall(function() return c:nothere() end) return ok, err')[0]);

// Every dispatch crossed the boundary and was counted.
var_dump($sandbox->stats()->phpCallsOut > 0);

$sandbox->close();

?>
--EXPECT--
int(5)
array(2) {
  [0]=>
  int(15)
  [1]=>
  int(5)
}
string(9) "counter@5"
bool(true)
bool(false)
bool(true)
