--TEST--
A PHP callback's arguments and its single return value convert in both directions
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Sandbox;

// One PHP return value becomes exactly one Lua value. The extension this
// replaces made a callback wrap even a single result in an array and warned
// when it did not, which made the common case the awkward one. Here an array is
// a table like any other value, and a script that wants several results out of
// one call unpacks the table it was handed.

$sandbox = new Sandbox();

$sandbox->registerLibrary('host', [
	'nothing' => static fn (): mixed => null,
	'yes' => static fn (): bool => true,
	'count' => static fn (): int => 42,
	'ratio' => static fn (): float => 1.5,
	'text' => static fn (): string => "caf\xc3\xa9\x00end",
	'list' => static fn (): array => [10, 20, 30],
	'map' => static fn (): array => ['a' => 1, 'b' => ['c' => 2]],
	'pair' => static fn (): array => ['first', 'second'],
	'echo' => static fn (mixed ...$arguments): array => [
		'count' => count($arguments),
		'values' => $arguments,
	],
]);

// Every convertible type arrives as the Lua type it should, with the integer
// and float distinction preserved rather than guessed at from the value.
var_dump($sandbox->eval(<<<'LUA'
	return type(host.nothing()), type(host.yes()), type(host.count()),
		math.type(host.count()), type(host.ratio()), math.type(host.ratio()),
		type(host.text()), type(host.list()), type(host.map())
LUA));

var_dump($sandbox->eval('return host.yes(), host.count(), host.ratio()'));

// Strings are bytes in both languages, so an embedded NUL is content.
var_dump(bin2hex($sandbox->eval('return host.text()')[0]));

// A returned PHP list keeps its own keys rather than being renumbered, exactly
// as it does through setGlobal() -- see 09-conversion/array-key-mapping.phpt for
// the reasoning. So a 0-indexed PHP list arrives with its first element sitting
// outside Lua's idea of the sequence, and # reports the shorter run.
var_dump($sandbox->eval('local t = host.list() return #t, t[0], t[2], t[3] == nil'));
var_dump($sandbox->eval('local m = host.map() return m.a, m.b.c'));

// One value, not two: table.unpack is how a script spreads what it was given.
// Its default range starts at 1, so it too works from the sequence, not from
// key 0 -- a script wanting the whole list passes explicit bounds.
var_dump($sandbox->eval('return select("#", host.pair())'));
var_dump($sandbox->eval('return table.unpack(host.pair())'));
var_dump($sandbox->eval('return table.unpack(host.pair(), 0, 1)'));

// Arguments travel the other way positionally, with their types intact.
var_dump($sandbox->eval(<<<'LUA'
	local r = host.echo(1, "two", 3.5, true)
	return r.count, r.values[0], r.values[1], r.values[2], r.values[3]
LUA));

// No arguments at all is not an error.
var_dump($sandbox->eval('return host.echo().count'));

// wrapCallable() hands the same bridge back as a LuaFunction, which the host
// can call directly or give to a script.
$doubler = $sandbox->wrapCallable(static fn (int $value): int => $value * 2, 'doubler');
var_dump($doubler(21));

$sandbox->setGlobal('doubler', $doubler);
var_dump($sandbox->eval('return doubler(50)'));

$sandbox->close();

?>
--EXPECT--
array(9) {
  [0]=>
  string(3) "nil"
  [1]=>
  string(7) "boolean"
  [2]=>
  string(6) "number"
  [3]=>
  string(7) "integer"
  [4]=>
  string(6) "number"
  [5]=>
  string(5) "float"
  [6]=>
  string(6) "string"
  [7]=>
  string(5) "table"
  [8]=>
  string(5) "table"
}
array(3) {
  [0]=>
  bool(true)
  [1]=>
  int(42)
  [2]=>
  float(1.5)
}
string(18) "636166c3a900656e64"
array(4) {
  [0]=>
  int(2)
  [1]=>
  int(10)
  [2]=>
  int(30)
  [3]=>
  bool(true)
}
array(2) {
  [0]=>
  int(1)
  [1]=>
  int(2)
}
array(1) {
  [0]=>
  int(1)
}
array(1) {
  [0]=>
  string(6) "second"
}
array(2) {
  [0]=>
  string(5) "first"
  [1]=>
  string(6) "second"
}
array(5) {
  [0]=>
  int(4)
  [1]=>
  int(1)
  [2]=>
  string(3) "two"
  [3]=>
  float(3.5)
  [4]=>
  bool(true)
}
array(1) {
  [0]=>
  int(0)
}
array(1) {
  [0]=>
  int(42)
}
array(1) {
  [0]=>
  int(100)
}
