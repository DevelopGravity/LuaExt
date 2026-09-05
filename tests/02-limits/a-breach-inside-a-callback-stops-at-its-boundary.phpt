--TEST--
A CPU breach inside a host callback is delivered at that callback's boundary
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Exception\CpuLimitError;
use DevelopGravity\LuaExt\Limits;
use DevelopGravity\LuaExt\Sandbox;
use DevelopGravity\LuaExt\SandboxConfig;

// The callback boundary used to read only the sticky flag, which the watchdog
// thread sets when it wakes. A script that crossed its deadline INSIDE a
// callback -- where no interpreter back edge runs -- and returned through
// straight-line Lua could keep going until the end-of-call check caught it:
// the exception still arrived, but only after the script had run whatever else
// it liked. Here "whatever else" is a second callback, and the assertion is
// that it never runs.
//
// The boundary now samples the deadline directly when the callback ran for at
// least a millisecond (the gate keeps trivial crossings at ~0.3us; see
// luaext_phpcall.c). The burn below runs ~30ms, so it is always sampled, and
// the overshoot is kept tiny so the watchdog thread usually loses the race --
// without the boundary sample, the second callback ran in 80 of 100
// iterations on an idle machine.

$sneakedPast = 0;
$stopped = 0;

for ($i = 0; $i < 100; $i++) {
	$sandbox = new Sandbox(new SandboxConfig(
		limits: new Limits(cpuSeconds: 0.03, wallClockSeconds: 5.0, billHostTime: true),
	));

	$ranAfterBreach = false;

	$sandbox->registerLibrary('host', [
		// Burns to just past the deadline, then returns immediately.
		'nudge' => static function () use ($sandbox): int {
			while ($sandbox->stats()->cpuSeconds < 0.032) {
				for ($spin = 0; $spin < 500; $spin++) {
				}
			}

			return 1;
		},

		// Must never run: by the time the script could call this, its budget
		// is already spent.
		'record' => static function () use (&$ranAfterBreach): int {
			$ranAfterBreach = true;

			return 1;
		},
	]);

	try {
		(void) $sandbox->eval('local a = host.nudge() local b = host.record() return b', '=race');
	} catch (CpuLimitError) {
	}

	if ($ranAfterBreach) {
		$sneakedPast++;
	} else {
		$stopped++;
	}

	$sandbox->close();
}

printf("stopped at the boundary: %d\n", $stopped);
printf("ran more work in breach: %d\n", $sneakedPast);

?>
--EXPECT--
stopped at the boundary: 100
ran more work in breach: 0
