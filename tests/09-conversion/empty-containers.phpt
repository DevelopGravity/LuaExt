--TEST--
Empty arrays and tables convert without touching a shared immutable array
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Sandbox;

// An empty array literal is not an ordinary array. The compiler hands out
// zend_empty_array, which is immutable and lives in read-only memory shared by
// every empty literal in the process, and a constant array from opcache sits in
// shared memory too. Anything that assumes a HashTable is refcountable --
// taking a reference to it, or setting a flag on it -- faults on those rather
// than failing cleanly, so every empty container is worth converting for real.
const EMPTY_CONSTANT = [];

$sandbox = new Sandbox();

$empties = [
	'literal' => [],
	'constant' => EMPTY_CONSTANT,
	'emptied' => array_filter([1, 2], static fn(int $number): bool => false),
	'nested' => ['inner' => [], 'list' => [[], []]],
];

foreach ($empties as $label => $value) {
	$sandbox->setGlobal('probe', $value);
	printf("%s: %s\n", $label, var_export($sandbox->getGlobal('probe') == $value, true));
}

// Pushed twice, so a first conversion that damaged the shared literal would
// show up on the second.
$sandbox->setGlobal('first', []);
$sandbox->setGlobal('second', []);
var_dump($sandbox->eval('return type(first), next(first) == nil, type(second), next(second) == nil'));

// The literal is still usable as a PHP value afterwards.
$after = [];
var_dump($after === [], count($after), json_encode([]));

// And the other direction: an empty Lua table is an empty PHP array, not null.
[$table] = $sandbox->eval('return {}');
var_dump($table, $table === []);

// Including empty tables nested inside a non-empty one.
[$nested] = $sandbox->eval('return { a = {}, b = { {}, {} } }');
var_dump($nested['a'], count($nested['b']), $nested['b'][1], $nested['b'][2]);

// A chunk that returns nothing is an empty list: the zero-count path builds a
// fresh array rather than handing back a borrowed one.
var_dump($sandbox->eval('return'), $sandbox->eval('local t = {} return table.unpack(t)'));

?>
--EXPECT--
literal: true
constant: true
emptied: true
nested: true
array(4) {
  [0]=>
  string(5) "table"
  [1]=>
  bool(true)
  [2]=>
  string(5) "table"
  [3]=>
  bool(true)
}
bool(true)
int(0)
string(2) "[]"
array(0) {
}
bool(true)
array(0) {
}
int(2)
array(0) {
}
array(0) {
}
array(0) {
}
array(0) {
}
