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
// With a freelist the cost is bounded by the number of handles ALIVE AT ONCE,
// not by the number ever created. Both halves of that sentence are measured
// here, and the second half is why:
//
//   A test that only showed "growth stays near zero" would also pass if the
//   measurement itself had stopped working -- a stats() that always answered 0
//   reads exactly like perfect reuse. So the same loop is run a second time
//   HOLDING every handle, where growth must appear. One run says reuse works;
//   the pair says the instrument works too.
//
// This used to create 200000 handles and assert an absolute byte ceiling. That
// passed everywhere, including under valgrind -- it just took two minutes there
// to demonstrate something two runs of 5000 demonstrate better, because the
// claim is about the SHAPE of the growth rather than its size.
const HANDLES = 5000;

$sandbox = new Sandbox();

// One handle first, so the measurement does not include the registry table's
// own creation.
$warmup = $sandbox->compile('return 1');
unset($warmup);

$baseline = $sandbox->stats()->memoryBytes;

// Assigning over the previous handle releases its slot before the next one is
// taken, so at most two are ever live.
$live = null;

for ($index = 0; $index < HANDLES; $index++) {
	$live = $sandbox->compile('return 1');
}

$afterFirstBatch = $sandbox->stats()->memoryBytes - $baseline;

// A second batch through the same slots. If the freelist works this costs
// nothing at all; without it, the refs table grows by another HANDLES entries.
for ($index = 0; $index < HANDLES; $index++) {
	$live = $sandbox->compile('return 1');
}

unset($live);

$afterSecondBatch = $sandbox->stats()->memoryBytes - $baseline;
$secondBatchCost = $afterSecondBatch - $afterFirstBatch;

// THE CONTROL. The same number of handles, all held at once, must cost real
// memory -- otherwise the two readings above prove nothing about reuse and only
// that nothing was being measured.
$held = [];

for ($index = 0; $index < HANDLES; $index++) {
	$held[] = $sandbox->compile('return 1');
}

$whileHeld = $sandbox->stats()->memoryBytes - $baseline;

unset($held);

printf("first batch is bounded  = %s\n", var_export($afterFirstBatch < 64 * 1024, true));
printf("second batch is free    = %s\n", var_export($secondBatchCost < 16 * 1024, true));
printf("holding them costs      = %s\n", var_export($whileHeld > 8 * $afterFirstBatch, true));

// And the sandbox still works afterwards: reused slots are cleared, not
// merely renumbered.
$function = $sandbox->compile('return "reused slot"');
var_dump($function->call());

$sandbox->close();

?>
--EXPECT--
first batch is bounded  = true
second batch is free    = true
holding them costs      = true
array(1) {
  [0]=>
  string(11) "reused slot"
}
