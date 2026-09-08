--TEST--
Boundary arguments are held to the callee's declared types under strict_types semantics
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\LuaFunction;
use DevelopGravity\LuaExt\LuaMethod;
use DevelopGravity\LuaExt\Sandbox;

trait Comparable
{
	#[LuaMethod]
	public function sameAs(self $other): bool
	{
		return $other instanceof self;
	}
}

final class Card
{
	use Comparable;

	#[LuaMethod]
	public function __construct(public readonly int $rank = 0) {}
}

class Ticket
{
	#[LuaMethod]
	public function __construct(public readonly int $id = 0) {}

	#[LuaMethod]
	public function id(): int { return $this->id; }
}

final class SubTicket extends Ticket
{
	#[LuaMethod]
	public function swap(parent $other): int
	{
		return $other->id;
	}
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

$sandbox = new Sandbox();
$sandbox->registerClass(Card::class);
$sandbox->registerClass(Ticket::class);
$sandbox->registerClass(SubTicket::class);
$sandbox->registerClass(Bag::class);

$sandbox->registerLibrary('t', [
	'take' => static fn (int $value): int => $value + 1,
	'widen' => static fn (float $ratio): float => $ratio * 2.0,
	'narrow' => static fn (int $count): int => $count,
	'flag' => static fn (bool $on): bool => !$on,
	'name' => static fn (string $name): string => strtoupper($name),
	'maybe' => static fn (?int $value): string => $value === null ? 'null' : 'int',
	'loose' => static fn ($anything) => 'ok',
	'any' => static fn (mixed $anything): string => 'ok',
	'many' => static fn (mixed ...$args): int => count($args),
	'ticket' => static fn (Ticket $ticket): int => $ticket->id,
	'ticketish' => static fn (Ticket|int $value): string => is_int($value) ? 'int' : 'ticket',
	'both' => static fn (Countable&ArrayAccess $bag): int => count($bag),
	'walk' => static fn (iterable $items): int => is_array($items) ? count($items) : -1,
	'nullableTicket' => static fn (?Ticket $ticket): string => $ticket === null ? 'null' : 'ticket',
	'run' => static fn (callable $fn): string => 'callable',
	'handle' => static fn (LuaFunction $fn): string => 'handle',
	'closure' => static fn (Closure $fn): string => 'closure',
	'pair' => static fn (float $a, int $b): float => $a + $b,
	'up' => 'strtoupper',
]);

$sandbox->setGlobal('card1', new Card(1));
$sandbox->setGlobal('card2', new Card(2));
$sandbox->setGlobal('tk', new Ticket(7));
$sandbox->setGlobal('sub', new SubTicket(8));
$sandbox->setGlobal('bag', new Bag(['a', 'b']));

$probe = static function (string $label, string $expression) use ($sandbox): void {
	$result = $sandbox->eval(
		"local ok, value = pcall(function() return {$expression} end) " .
		'if ok then return "ok", tostring(value) end return "err", tostring(value)',
		'=probe',
	);

	printf("%-18s %s: %s\n", $label, $result[0], $result[1]);
};

// The strict scalar map: exact types, plus the one sanctioned widening.
$probe('int-ok', 't.take(41)');
$probe('string-to-int', 't.take("41")');
$probe('float-to-int', 't.narrow(2.5)');
$probe('whole-float-to-int', 't.narrow(3.0)');
$probe('bool-to-int', 't.take(true)');
$probe('int-to-float', 't.widen(2)');
$probe('int-to-string', 't.name(42)');
$probe('bool-ok', 't.flag(false)');
$probe('int-to-bool', 't.flag(1)');

// Explicit nil is null; nullable accepts it, non-nullable refuses it.
$probe('nil-nullable', 't.maybe(nil)');
$probe('nil-not-nullable', 't.take(nil)');

// Nothing declared, nothing enforced.
$probe('untyped', 't.loose(true)');
$probe('mixed', 't.any({})');
$probe('variadic-mixed', 't.many(1, "two", {}, nil)');

// Class contracts: the right proxy passes, everything else refuses.
$probe('class-ok', 't.ticket(tk)');
$probe('class-subclass', 't.ticket(sub)');
$probe('class-wrong', 't.ticket(card1)');
$probe('class-scalar', 't.ticket(7)');
$probe('union-ticket', 't.ticketish(tk)');
$probe('union-int', 't.ticketish(5)');
$probe('union-string', 't.ticketish("5")');
$probe('intersection-ok', 't.both(bag)');
$probe('intersection-no', 't.both(tk)');
$probe('iterable-table', 't.walk({1, 2, 3})');
$probe('nullable-class', 't.nullableTicket(nil)');

// Trait-declared self and a subclass's parent both resolve where written.
$probe('trait-self-ok', 'card1:sameAs(card2)');
$probe('trait-self-wrong', 'card1:sameAs(tk)');
$probe('parent-ok', 'sub:swap(tk)');
$probe('parent-wrong', 'sub:swap(card1)');

// Functions cross as LuaFunction: invokable, so callable-typed params take
// them; Closure-typed params are a different, unsatisfied contract.
$probe('callable-fn', 't.run(function() end)');
$probe('luafunction', 't.handle(function() end)');
$probe('closure', 't.closure(function() end)');

// Internal functions carry arginfo too.
$probe('internal-ok', 't.up("abc")');
$probe('internal-wrong', 't.up(1)');

// Constructor arguments cross the same gate.
$probe('new-ok', 'Ticket.new(3):id()');
$probe('new-wrong', 'Ticket.new("3")');

// Widen-then-refuse: the first argument's in-place int->float widening must
// strand nothing when the second refuses -- and a refused call is not a
// crossing, so phpCallsOut stays put exactly. Memory symmetry is proven
// across fifty refusals: a leaked argument charge would compound; ambient
// Lua-GC residue does not.
$probe('widen-refuse', 't.pair(1, "x")');
(void) $sandbox->eval('collectgarbage("collect")');
$callsBefore = $sandbox->stats()->phpCallsOut;
$memoryBefore = $sandbox->stats()->memoryBytes;
(void) $sandbox->eval('for i = 1, 50 do pcall(function() return t.pair(1, "x") end) end collectgarbage("collect")');
var_dump($sandbox->stats()->phpCallsOut === $callsBefore);
var_dump($sandbox->stats()->memoryBytes - $memoryBefore < 16384);

$sandbox->close();

?>
--EXPECT--
int-ok             ok: 42
string-to-int      err: take: argument #1 ($value) must be of type int, string given
float-to-int       err: narrow: argument #1 ($count) must be of type int, float given
whole-float-to-int err: narrow: argument #1 ($count) must be of type int, float given
bool-to-int        err: take: argument #1 ($value) must be of type int, true given
int-to-float       ok: 4.0
int-to-string      err: name: argument #1 ($name) must be of type string, int given
bool-ok            ok: true
int-to-bool        err: flag: argument #1 ($on) must be of type bool, int given
nil-nullable       ok: null
nil-not-nullable   err: take: argument #1 ($value) must be of type int, null given
untyped            ok: ok
mixed              ok: ok
variadic-mixed     ok: 4
class-ok           ok: 7
class-subclass     ok: 8
class-wrong        err: ticket: argument #1 ($ticket) must be of type Ticket, Card given
class-scalar       err: ticket: argument #1 ($ticket) must be of type Ticket, int given
union-ticket       ok: ticket
union-int          ok: int
union-string       err: ticketish: argument #1 ($value) must be of type Ticket|int, string given
intersection-ok    ok: 2
intersection-no    err: both: argument #1 ($bag) must be of type Countable&ArrayAccess, Ticket given
iterable-table     ok: 3
nullable-class     ok: null
trait-self-ok      ok: true
trait-self-wrong   err: sameAs: argument #1 ($other) must be of type self, Ticket given
parent-ok          ok: 7
parent-wrong       err: swap: argument #1 ($other) must be of type Ticket, Card given
callable-fn        ok: callable
luafunction        ok: handle
closure            err: closure: argument #1 ($fn) must be of type Closure, DevelopGravity\LuaExt\LuaFunction given
internal-ok        ok: ABC
internal-wrong     err: up: argument #1 ($string) must be of type string, int given
new-ok             ok: 3
new-wrong          err: new: argument #1 ($id) must be of type int, string given
widen-refuse       err: pair: argument #2 ($b) must be of type int, string given
bool(true)
bool(true)
