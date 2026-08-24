--TEST--
Array keys keep their type and value across the boundary
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Sandbox;

$sandbox = new Sandbox();

// PHP lists are 0-indexed and Lua sequences are 1-indexed. Nothing is
// renumbered on the way through -- key 0 stays key 0 -- so a PHP list becomes
// a table whose first element sits outside Lua's idea of the sequence. That is
// the honest mapping, and the length operator shows it.
$sandbox->setGlobal('list', ['a', 'b', 'c']);
var_dump($sandbox->eval('return #list, list[0], list[1], list[2], list[3] == nil'));

// Mixed keys survive with their types intact.
$mixed = [
	0 => 'zero',
	7 => 'seven',
	-1 => 'minus one',
	'name' => 'string key',
	'7 ' => 'not numeric',
];

$sandbox->setGlobal('mixed', $mixed);
var_dump($sandbox->eval('return mixed[0], mixed[7], mixed[-1], mixed.name, mixed["7 "]'));

$returned = $sandbox->getGlobal('mixed');
ksort($returned, SORT_STRING);
ksort($mixed, SORT_STRING);
var_dump($returned === $mixed);

// PHP folds the numeric string "7" onto the integer key 7 when the array is
// built, so there is only ever one of them to push; the collision case can
// only arise on the way back from Lua.
$folded = ['7' => 'folded'];
var_dump(array_keys($folded));
$sandbox->setGlobal('folded', $folded);
var_dump($sandbox->eval('return folded[7], folded["7"] == nil'));

// An empty array is an empty table, not nil.
$sandbox->setGlobal('empty', []);
var_dump($sandbox->eval('return type(empty), next(empty) == nil'));
var_dump($sandbox->getGlobal('empty'));

?>
--EXPECT--
array(5) {
  [0]=>
  int(2)
  [1]=>
  string(1) "a"
  [2]=>
  string(1) "b"
  [3]=>
  string(1) "c"
  [4]=>
  bool(true)
}
array(5) {
  [0]=>
  string(4) "zero"
  [1]=>
  string(5) "seven"
  [2]=>
  string(9) "minus one"
  [3]=>
  string(10) "string key"
  [4]=>
  string(11) "not numeric"
}
bool(true)
array(1) {
  [0]=>
  int(7)
}
array(2) {
  [0]=>
  string(6) "folded"
  [1]=>
  bool(true)
}
array(2) {
  [0]=>
  string(5) "table"
  [1]=>
  bool(true)
}
array(0) {
}
