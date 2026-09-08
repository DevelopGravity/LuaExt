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

// The callable route pins the rich signature shapes in boundary-callee-shapes;
// these carry the same shapes through proxy dispatch, which shares the core.
final class Gauge
{
	#[LuaMethod]
	public function __construct(public int $value = 0) {}

	#[LuaMethod]
	public static function sum(int ...$values): int { return array_sum($values); }

	#[LuaMethod]
	public function label(?string $name): string { return $name ?? '(none)'; }

	#[LuaMethod]
	public function pick(int|string $key): string { return (string) $key; }

	#[LuaMethod]
	public function walk(Traversable&Countable $items): int { return count($items); }

	#[LuaMethod]
	public function pad(int $value, int $width = 4): string
	{
		return str_pad((string) $value, $width, '0', STR_PAD_LEFT);
	}

	#[LuaMethod]
	public function fill(int &$out): void { $out = 1; }

	#[LuaMethod]
	public function poke() { return 7; }
}

$sandbox = new Sandbox();
$sandbox->registerClass(Meter::class);
$sandbox->registerClass(Gauge::class);

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

// A mixed operand never reaches the mapped method: the identity check names
// the shapes, and the message is part of the contract.
$probe('operator-mixed', 'Meter.of(2) * 5');

// Variadics accept any surplus through a static and type-check every one.
$probe('variadic-ok', 'Gauge.sum(1, 2, 3)');
$probe('variadic-wrong', 'Gauge.sum(1, "2")');

// Nullable, union, and intersection parameters through instance dispatch.
$probe('nullable-ok', 'Gauge.new(1):label(nil)');
$probe('nullable-wrong', 'Gauge.new(1):label(5)');
$probe('union-ok', 'Gauge.new(1):pick(7)');
$probe('union-wrong', 'Gauge.new(1):pick(true)');
$probe('intersection', 'Gauge.new(1):walk(5)');

// A defaulted optional fills in; surplus past the declared list is refused.
$probe('default-ok', 'Gauge.new(1):pad(7)');
$probe('default-extra', 'Gauge.new(1):pad(7, 2, 9)');

// By-reference cannot cross, and a callee declaring nothing still owes arity.
$probe('byref', 'Gauge.new(1):fill(5)');
$probe('bare-ok', 'Gauge.new(1):poke()');
$probe('bare-extra', 'Gauge.new(1):poke(1)');

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
operator-mixed     err: cannot apply '*' to Meter and number
variadic-ok        ok: 6
variadic-wrong     err: sum: argument #2 ($values) must be of type int, string given
nullable-ok        ok: (none)
nullable-wrong     err: label: argument #1 ($name) must be of type ?string, int given
union-ok           ok: 7
union-wrong        err: pick: argument #1 ($key) must be of type string|int, true given
intersection       err: walk: argument #1 ($items) must be of type Traversable&Countable, int given
default-ok         ok: 0007
default-extra      err: pad expects at most 2 argument(s), 3 given
byref              err: fill: argument #1 ($out) is passed by reference, which cannot cross from Lua
bare-ok            ok: 7
bare-extra         err: poke expects at most 0 argument(s), 1 given
