--TEST--
A self-referential Lua table is refused, and a shared subtree is not
--EXTENSIONS--
luaext
--XFAIL--
Needs Sandbox::eval(), which lands with the execution subsystem.
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Exception\ConversionError;
use DevelopGravity\LuaExt\Sandbox;

$sandbox = new Sandbox();

$cycles = [
	'direct' => 'local t = { name = "root" }; t.self = t; return t',
	'indirect' => 'local a, b = {}, {}; a.down = b; b.back = a; return a',
	'through a key' => 'local t = {}; t[1] = { t }; return t',
];

foreach ($cycles as $label => $chunk) {
	try {
		printf("NOT REFUSED %s: %s\n", $label, var_export($sandbox->eval($chunk), true));
	} catch (ConversionError $error) {
		printf("%-14s %s circular=%s\n",
			$label,
			$error::class,
			var_export(str_contains($error->getMessage(), 'circular'), true));
	}
}

// Reaching the same table twice by different paths is a shared subtree, not a
// cycle: it has a perfectly good PHP representation, one copy per path.
[$graph] = $sandbox->eval('local shared = { count = 1 }; return { a = shared, b = shared }');
var_dump($graph['a'], $graph['b'], $graph['a'] === $graph['b']);

// So is the same table appearing twice in a sequence. Lua sequences start at
// 1, and the keys arrive as the integers Lua used rather than renumbered.
[$twice] = $sandbox->eval('local shared = { 1 }; return { shared, shared }');
$keys = array_keys($twice);
sort($keys);
var_dump(count($twice), $keys, $twice[1], $twice[2]);

// A refusal leaves the interpreter usable and its stack balanced.
var_dump($sandbox->eval('return 1, 2, 3'));

?>
--EXPECT--
direct         DevelopGravity\LuaExt\Exception\ConversionError circular=true
indirect       DevelopGravity\LuaExt\Exception\ConversionError circular=true
through a key  DevelopGravity\LuaExt\Exception\ConversionError circular=true
array(1) {
  ["count"]=>
  int(1)
}
array(1) {
  ["count"]=>
  int(1)
}
bool(true)
int(2)
array(2) {
  [0]=>
  int(1)
  [1]=>
  int(2)
}
array(1) {
  [1]=>
  int(1)
}
array(1) {
  [1]=>
  int(1)
}
array(3) {
  [0]=>
  int(1)
  [1]=>
  int(2)
  [2]=>
  int(3)
}
