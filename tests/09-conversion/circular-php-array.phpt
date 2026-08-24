--TEST--
A self-referential PHP array is refused, and the array is left untouched
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Exception\ConversionError;
use DevelopGravity\LuaExt\Sandbox;

$sandbox = new Sandbox();

// The only way a PHP array can contain itself is through a reference, and the
// dereferenced array is the same zend_array the walk already passed through.
$direct = ['name' => 'root'];
$direct['self'] = &$direct;

try {
	$sandbox->setGlobal('cycle', $direct);
	echo "NOT REFUSED\n";
} catch (ConversionError $error) {
	printf("%s circular=%s\n",
		$error::class,
		var_export(str_contains($error->getMessage(), 'circular'), true));
}

// Indirect cycles too: the loop closes two levels down.
$outer = ['level' => 1];
$inner = ['level' => 2];
$inner['back'] = &$outer;
$outer['down'] = $inner;

try {
	$sandbox->setGlobal('cycle', $outer);
	echo "NOT REFUSED\n";
} catch (ConversionError $error) {
	printf("%s circular=%s\n",
		$error::class,
		var_export(str_contains($error->getMessage(), 'circular'), true));
}

// Detection must not leave the array marked as recursive behind it: PHP's own
// recursion protection lives on the same structure, and a stale flag would
// make print_r report a cycle that is no longer there.
print_r($direct['name']);
echo "\n";
var_dump(json_encode(['level' => $outer['level'], 'down' => $outer['down']['level']]));

// A shared subtree is not a cycle and still converts.
$shared = ['count' => 1];
$sandbox->setGlobal('graph', ['a' => $shared, 'b' => $shared]);
var_dump($sandbox->eval('return graph.a.count + graph.b.count'));

// The sandbox survives a refusal; no half-built table is left behind.
var_dump($sandbox->eval('return type(cycle)'));

?>
--EXPECT--
DevelopGravity\LuaExt\Exception\ConversionError circular=true
DevelopGravity\LuaExt\Exception\ConversionError circular=true
root
string(20) "{"level":1,"down":2}"
array(1) {
  [0]=>
  int(2)
}
array(1) {
  [0]=>
  string(3) "nil"
}
