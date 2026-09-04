--TEST--
Released registry slots are handed out again instead of leaking upward
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Sandbox;

// Every LuaFunction holds a slot in its sandbox's registry table. The
// extension this replaces only ever incremented its counter, so a long-lived
// worker sandbox that compiled and dropped functions walked that counter
// toward INT_MAX and grew a registry table full of holes behind it.
//
// With a freelist the cost is bounded by the number of handles alive at once,
// not by the number ever created -- which is exactly what a flat memory
// reading across many sequential handles shows.
const HANDLES = 200000;

$sandbox = new Sandbox();

// One handle first, so the measurement does not include the registry table's
// own creation.
$warmup = $sandbox->compile('return 1');
unset($warmup);

$baseline = $sandbox->stats()->memoryBytes;
$live = null;

for ($index = 0; $index < HANDLES; $index++) {
	// Assigning over the previous handle releases its slot before the next one
	// is taken, so at most two are ever live.
	$live = $sandbox->compile('return 1');
}

unset($live);

$growth = $sandbox->stats()->memoryBytes - $baseline;

// Without reuse the refs table alone would carry 200000 entries, which is over
// a megabyte before anything the handles point at is counted.
printf("bounded=%s\n", var_export($growth < 256 * 1024, true));

// And the sandbox still works afterwards: reused slots are cleared, not
// merely renumbered.
$function = $sandbox->compile('return "reused slot"');
var_dump($function->call());

?>
--EXPECT--
bounded=true
array(1) {
  [0]=>
  string(11) "reused slot"
}
