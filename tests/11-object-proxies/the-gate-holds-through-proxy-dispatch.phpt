--TEST--
The strict argument gate holds through constructor, static, and operator dispatch
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\LuaMethod;
use DevelopGravity\LuaExt\LuaOperator;
use DevelopGravity\LuaExt\Operator;
use DevelopGravity\LuaExt\Sandbox;

// The gate was landed on the callable boundary; every proxy dispatch site
// routes through the same core, and this pins that none of them bypasses it.
final class Meter
{
	#[LuaMethod]
	public function __construct(public readonly int $value = 0) {}

	#[LuaMethod]
	public static function of(int $value): self { return new self($value); }

	#[LuaMethod]
	public function plus(self $other): int { return $this->value + $other->value; }

	#[LuaMethod]
	public function get(): int { return $this->value; }

	#[LuaOperator(Operator::Multiply)]
	public function times(self $other): self { return new self($this->value * $other->value); }
}

$sandbox = new Sandbox();
$sandbox->registerClass(Meter::class);

$probe = static function (string $label, string $expression) use ($sandbox): void {
	$result = $sandbox->eval(
		"local ok, value = pcall(function() return {$expression} end) " .
		'if ok then return "ok", tostring(value) end return "err", tostring(value)',
		'=probe',
	);

	printf("%-18s %s: %s\n", $label, $result[0], $result[1]);
};

$probe('ctor-ok', 'Meter.new(3):get()');
$probe('ctor-wrong', 'Meter.new("3")');

$probe('static-ok', 'Meter.of(4):get()');
$probe('static-wrong', 'Meter.of("4")');
$probe('static-arity', 'Meter.of()');

$probe('method-ok', 'Meter.of(2):plus(Meter.of(3))');
$probe('method-wrong', 'Meter.of(2):plus(5)');

$probe('operator-ok', '(Meter.of(2) * Meter.of(3)):get()');

$sandbox->close();

?>
--EXPECT--
ctor-ok            ok: 3
ctor-wrong         err: new: argument #1 ($value) must be of type int, string given
static-ok          ok: 4
static-wrong       err: of: argument #1 ($value) must be of type int, string given
static-arity       err: of expects at least 1 argument(s), 0 given
method-ok          ok: 5
method-wrong       err: plus: argument #1 ($other) must be of type Meter, int given
operator-ok        ok: 6
