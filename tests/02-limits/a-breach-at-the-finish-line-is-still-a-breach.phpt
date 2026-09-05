--TEST--
A call that crosses its CPU deadline just before returning still reports the breach
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Exception\CpuLimitError;
use DevelopGravity\LuaExt\Limits;
use DevelopGravity\LuaExt\Sandbox;
use DevelopGravity\LuaExt\SandboxConfig;

// Delivery through the watchdog thread is a race the thread can lose. The
// thread wakes at the deadline and sets the sticky flag; the flag is read at
// interpreter back edges and at the call boundaries. A script that crosses its
// deadline just before returning can be back in PHP before the thread's next
// wakeup -- and the return boundary used to read ONLY the flag, then disarm
// the slot, so the late wakeup found nothing to service and a measured breach
// reported success.
//
// This arranges that race on purpose: the overshoot happens inside a PHP
// callback, where no back edge runs, and the return path is a straight line of
// Lua with no back edge either. The only things standing between a breach and
// a clean return are the two boundary checks -- which now sample the deadline
// directly instead of trusting the thread to have won.
//
// One iteration is a coin flip on thread scheduling, so it runs a hundred:
// with the boundary sampling every one reports the breach, deterministically.
// Without it, an idle development machine escaped 60 of 200 -- the chance of
// an unfixed build passing a hundred straight is (~0.7)^100.

$escapes = 0;
$caught = 0;

for ($i = 0; $i < 100; $i++) {
	$sandbox = new Sandbox(new SandboxConfig(
		limits: new Limits(cpuSeconds: 0.03, wallClockSeconds: 5.0, billHostTime: true),
	));

	$sandbox->registerLibrary('host', [
		// Burns to just past the deadline, then stops immediately: the smaller
		// the overshoot, the likelier the watchdog thread is still asleep.
		'nudge' => static function () use ($sandbox): int {
			while ($sandbox->stats()->cpuSeconds < 0.032) {
				for ($spin = 0; $spin < 500; $spin++) {
				}
			}

			return 1;
		},
	]);

	try {
		(void) $sandbox->eval('local a = host.nudge() return a', '=race');
		$escapes++;
	} catch (CpuLimitError) {
		$caught++;
	}

	$sandbox->close();
}

printf("caught: %d\n", $caught);
printf("escaped in breach: %d\n", $escapes);

?>
--EXPECT--
caught: 100
escaped in breach: 0
