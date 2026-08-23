--TEST--
Nested arrays become nested tables and come back the same shape
--EXTENSIONS--
luaext
--XFAIL--
Needs Sandbox::eval()/setGlobal()/getGlobal(), which land with the execution subsystem.
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Sandbox;

/**
 * Table iteration order is not defined in Lua, so a round trip is compared on
 * keys and values rather than on ordering.
 */
function normalise(array $array): array
{
	ksort($array, SORT_STRING);

	foreach ($array as $key => $value) {
		if (is_array($value)) {
			$array[$key] = normalise($value);
		}
	}

	return $array;
}

$data = [
	'name' => 'root',
	'numbers' => [1, 2, 3],
	'nested' => ['deep' => ['deeper' => ['value' => 42, 'flag' => false]]],
	'empty' => [],
];

$sandbox = new Sandbox();
$sandbox->setGlobal('data', $data);

// Read through Lua so the assertion is about the table that actually exists in
// the interpreter, not about a value PHP handed back to itself.
var_dump($sandbox->eval(<<<'LUA'
	return data.name,
		data.numbers[0],
		data.numbers[2],
		data.nested.deep.deeper.value,
		data.nested.deep.deeper.flag,
		type(data.empty),
		next(data.empty) == nil
	LUA));

var_dump(normalise($sandbox->getGlobal('data')) === normalise($data));

// A subtree reachable by two paths is not a cycle: it converts into two
// tables, one per path.
$shared = ['count' => 1];
$sandbox->setGlobal('graph', ['left' => $shared, 'right' => $shared]);
var_dump($sandbox->eval('return graph.left.count + graph.right.count, graph.left == graph.right'));

?>
--EXPECT--
array(7) {
  [0]=>
  string(4) "root"
  [1]=>
  int(1)
  [2]=>
  int(3)
  [3]=>
  int(42)
  [4]=>
  bool(false)
  [5]=>
  string(5) "table"
  [6]=>
  bool(true)
}
bool(true)
array(2) {
  [0]=>
  int(2)
  [1]=>
  bool(false)
}
