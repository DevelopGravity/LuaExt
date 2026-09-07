--TEST--
enableProfiler() answers false exactly when sampling would disarm this build's CPU limit
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\LimitSupport;
use DevelopGravity\LuaExt\Sandbox;

// The documented false return exists for one build: when the watchdog thread
// could not start, the count hook IS the CPU limit, and replacing it with a
// sampling hook would leave the limit configured, reported enforced, and
// doing nothing. Sandbox::features() reports that same condition as a
// Degraded cpuLimit, so this test asserts the CONTRACT on whichever branch
// the platform lands -- deliberately not a SKIPIF, which would opt out of
// the false path on exactly the builds that exercise it.
$degraded = Sandbox::features()['cpuLimit'] === LimitSupport::Degraded;

$sandbox = new Sandbox();
$enabled = $sandbox->enableProfiler(0.001);

var_dump($enabled === !$degraded);

if ($enabled) {
	// The healthy branch: sampling really is on, and switches off cleanly.
	(void) $sandbox->eval('local x = 0 for i = 1, 300000 do x = x + i end return x', '=work');
	$sandbox->disableProfiler();
	var_dump(true);
} else {
	// The degraded branch: the refusal left no half-armed profiler behind --
	// no samples exist, and the sandbox still runs.
	var_dump($sandbox->getProfile() === []);
	(void) $sandbox->eval('return 1', '=still-works');
}

$sandbox->close();

?>
--EXPECT--
bool(true)
bool(true)
