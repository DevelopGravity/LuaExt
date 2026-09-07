--TEST--
cpuSeconds and wallClockSeconds are measured even when no timing limit is configured
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Limits;
use DevelopGravity\LuaExt\Sandbox;
use DevelopGravity\LuaExt\SandboxConfig;

// Measurement used to be a side effect of enforcement: the timing segments
// only opened when a limit was armed, so a sandbox built with both timing
// limits lifted reported 0.0 CPU and 0.0 wall forever, no matter what it
// burned -- and the stub sells these fields as billing-grade figures. The
// segments now open on every outermost call; only the watchdog thread and
// the deadline math stay tied to having something to enforce.
$sandbox = new Sandbox(new SandboxConfig(
	limits: new Limits(cpuSeconds: null, wallClockSeconds: null),
));

// Enough work that even a 15.6 ms Windows thread-clock tick registers it.
(void) $sandbox->eval('
	local x = 0
	for i = 1, 10000000 do
		x = x + i % 7
	end
	return x
', '=burn');

$stats = $sandbox->stats();

var_dump($stats->cpuSeconds > 0.0, $stats->wallClockSeconds > 0.0);

// Monotonic across calls, exactly as a limit-armed sandbox accumulates.
(void) $sandbox->eval('local x = 0 for i = 1, 1000000 do x = x + i end return x', '=more');

$after = $sandbox->stats();

var_dump(
	$after->cpuSeconds >= $stats->cpuSeconds,
	$after->wallClockSeconds > $stats->wallClockSeconds,
);

$sandbox->close();

?>
--EXPECT--
bool(true)
bool(true)
bool(true)
bool(true)
