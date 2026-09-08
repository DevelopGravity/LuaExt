--TEST--
The strict gate holds through every callee shape the engine distinguishes
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\LuaMethod;
use DevelopGravity\LuaExt\Sandbox;

class Ticket
{
	#[LuaMethod]
	public function __construct(public readonly int $id = 0) {}

	#[LuaMethod]
	public function id(): int { return $this->id; }
}

final class Card
{
	#[LuaMethod]
	public function __construct(public readonly int $rank = 0) {}
}

final class Bag implements Countable, ArrayAccess
{
	#[LuaMethod]
	public function __construct(private array $items = []) {}

	public function count(): int { return count($this->items); }
	public function offsetExists(mixed $offset): bool { return isset($this->items[$offset]); }
	public function offsetGet(mixed $offset): mixed { return $this->items[$offset] ?? null; }
	public function offsetSet(mixed $offset, mixed $value): void {}
	public function offsetUnset(mixed $offset): void {}
}

final class Walker implements IteratorAggregate
{
	#[LuaMethod]
	public function __construct(private array $items = []) {}

	public function getIterator(): Traversable { return new ArrayIterator($this->items); }
}

$sandbox = new Sandbox();
$sandbox->registerClass(Ticket::class);
$sandbox->registerClass(Card::class);
$sandbox->registerClass(Bag::class);
$sandbox->registerClass(Walker::class);

$sandbox->registerLibrary('t', [
	// `object` accepts any object and nothing else.
	'obj' => static fn (object $x): string => get_debug_type($x),

	// iterable = Traversable|array; both halves must pass, a scalar must not.
	'walk' => static fn (iterable $items): int => is_array($items)
		? count($items) : iterator_count($items),

	// A DNF union of an intersection and a name: the recursion the gate
	// nests exactly one level for.
	'pick' => static fn ((Countable&ArrayAccess)|Ticket $x): string => $x instanceof Ticket
		? 'ticket' : 'bag',

	// int must survive an int|float union unwidened; a float variadic must
	// widen every integer it is fed.
	'keep' => static fn (int|float $x): string => get_debug_type($x),
	'spread' => static fn (float ...$xs): string => implode(',', array_map(get_debug_type(...), $xs)),

	// An explicit nil is an argument, not an omission.
	'padded' => static fn (int $a, int $b = 7): int => $a + $b,

	// The IS_ARRAY branch of the callable check.
	'call' => static fn (callable $fn): mixed => $fn(),

	// Internal callees go through the same gate: arity, types, prefer-ref.
	'upper' => 'strtoupper',
	'cur' => 'current',
]);

$sandbox->setGlobal('tk', new Ticket(7));
$sandbox->setGlobal('cd', new Card(1));
$sandbox->setGlobal('bag', new Bag(['a', 'b']));
$sandbox->setGlobal('walker', new Walker([1, 2, 3]));

$probe = static function (string $label, string $expression) use ($sandbox): void {
	$result = $sandbox->eval(
		"local ok, value = pcall(function() return {$expression} end) " .
		'if ok then return "ok", tostring(value) end return "err", tostring(value)',
		'=probe',
	);

	printf("%-22s %s: %s\n", $label, $result[0], $result[1]);
};

$probe('object-proxy', 't.obj(tk)');
$probe('object-scalar', 't.obj(42)');

$probe('iterable-traversable', 't.walk(walker)');
$probe('iterable-scalar', 't.walk(5)');

$probe('dnf-intersection', 't.pick(bag)');
$probe('dnf-name', 't.pick(tk)');
$probe('dnf-neither', 't.pick(cd)');

$probe('union-int-stays-int', 't.keep(5)');
$probe('union-float', 't.keep(5.5)');
$probe('variadic-widens', 't.spread(1, 2.5)');

$probe('nil-into-optional', 't.padded(5, nil)');

$probe('array-callable', 't.call({[0] = tk, [1] = "id"})');
$probe('array-not-callable', 't.call({[0] = 1, [1] = 2})');

$probe('internal-type', 't.upper(5)');
$probe('internal-arity-over', 't.upper("a", "b")');
$probe('internal-arity-under', 't.upper()');
$probe('internal-prefer-ref', 't.cur({5, 6, 7})');

$sandbox->close();

?>
--EXPECT--
object-proxy           ok: Ticket
object-scalar          err: obj: argument #1 ($x) must be of type object, int given
iterable-traversable   ok: 3
iterable-scalar        err: walk: argument #1 ($items) must be of type Traversable|array, int given
dnf-intersection       ok: bag
dnf-name               ok: ticket
dnf-neither            err: pick: argument #1 ($x) must be of type (Countable&ArrayAccess)|Ticket, Card given
union-int-stays-int    ok: int
union-float            ok: float
variadic-widens        ok: float,float
nil-into-optional      err: padded: argument #2 ($b) must be of type int, null given
array-callable         ok: 7
array-not-callable     err: call: argument #1 ($fn) must be of type callable, array given
internal-type          err: upper: argument #1 ($string) must be of type string, int given
internal-arity-over    err: upper expects at most 1 argument(s), 2 given
internal-arity-under   err: upper expects at least 1 argument(s), 0 given
internal-prefer-ref    ok: 5
