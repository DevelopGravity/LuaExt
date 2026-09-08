--TEST--
Every proxy dispatch site bills phpCallsOut exactly once per crossing
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\LuaMethod;
use DevelopGravity\LuaExt\LuaOperator;
use DevelopGravity\LuaExt\Operator;
use DevelopGravity\LuaExt\Sandbox;

final class Billed
{
	#[LuaMethod]
	public function __construct(public readonly int $value = 0) {}

	#[LuaMethod]
	public function get(): int { return $this->value; }

	#[LuaMethod]
	public static function unit(): string { return 'b'; }

	#[LuaOperator(Operator::Add)]
	public function plus(self $other): self { return new self($this->value + $other->value); }

	#[LuaMethod]
	public function __toString(): string { return "billed:{$this->value}"; }
}

$sandbox = new Sandbox();
$sandbox->registerClass(Billed::class);

// Five crossings, one per dispatch site: constructor, instance method,
// static, mapped operator, and the __tostring metamethod. A loose "> 0"
// would pass on a double-billed or never-billed site; the exact delta is
// the assertion.
$before = $sandbox->stats()->phpCallsOut;
$wallBefore = $sandbox->stats()->phpWallClockSeconds;

(void) $sandbox->eval(<<<'LUA'
	local a = Billed.new(2)
	local g = a:get()
	local u = Billed.unit()
	local s = a + a
	local t = tostring(a)
LUA, '=five');

var_dump($sandbox->stats()->phpCallsOut - $before);
var_dump($sandbox->stats()->phpWallClockSeconds >= $wallBefore);

$sandbox->close();

?>
--EXPECT--
int(5)
bool(true)
