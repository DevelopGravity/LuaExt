--TEST--
stats() reports the CPU and wall budgets the limits enforce
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Limits;
use DevelopGravity\LuaExt\Sandbox;
use DevelopGravity\LuaExt\SandboxConfig;

/*
 * These two numbers are only useful if they describe exactly the quantities the
 * limits apply to. A usage counter measuring something adjacent -- process CPU
 * rather than thread CPU, or wall time including the pauses -- would be worse
 * than none at all, because a host would size its limits against it.
 *
 * Nothing here asserts a duration. Every assertion is an ordering or a
 * relationship that has to hold on a 1 ns clock and a 15.6 ms one alike.
 */

$sandbox = new Sandbox(new SandboxConfig(
	limits: (new Limits())->with(cpuSeconds: 5.0, wallClockSeconds: 10.0, billHostTime: true),
));

// A fresh sandbox has consumed nothing: construction happens before the first
// entry into the interpreter, and only entries are billed.
var_dump($sandbox->stats()->cpuSeconds === 0.0);
var_dump($sandbox->stats()->wallClockSeconds === 0.0);

// Reading a counter is not itself billable work.
$idle = $sandbox->stats()->cpuSeconds;
var_dump($sandbox->stats()->cpuSeconds === $idle);

(void) $sandbox->eval('local x = 0 for i = 1, 2000000 do x = x + i end return x', '=work');

$afterWork = $sandbox->stats()->cpuSeconds;

var_dump($afterWork > 0.0);

// Cumulative across calls, not per-call. A counter that reset on entry would
// make the limit a per-call one, and a host could run unbounded work by
// splitting it up.
(void) $sandbox->eval('local x = 0 for i = 1, 2000000 do x = x + i end return x', '=more');
var_dump($sandbox->stats()->cpuSeconds > $afterWork);

// Both counters moved, and they are separate counters. Deliberately NOT
// asserted here: that wall time is at least CPU time. It reads like an
// invariant and is not one for a CPU-bound loop -- macOS derives thread CPU
// time from scheduler accounting at microsecond granularity, so over a short
// busy loop it can land a hair above the monotonic elapsed time. The two are
// compared below instead, where a sleep puts a real gap between them.
var_dump($sandbox->stats()->wallClockSeconds > 0.0);

$sandbox->close();

/* -------------------------------------------------------------------------
 * What a pause does to the counters
 * ---------------------------------------------------------------------- */

$paused = new Sandbox(new SandboxConfig(
	limits: (new Limits())->with(cpuSeconds: 5.0, wallClockSeconds: 10.0, billHostTime: true),
));

$observed = [];

$paused->registerLibrary('host', [
	'sleepPaused' => static function () use ($paused, &$observed): void {
		$paused->pauseTimers();

		$before = $paused->stats()->wallClockSeconds;
		usleep(200_000);
		$observed['while paused'] = $paused->stats()->wallClockSeconds - $before;

		$paused->resumeTimers();

		$before = $paused->stats()->wallClockSeconds;
		usleep(200_000);
		$observed['while running'] = $paused->stats()->wallClockSeconds - $before;
	},
]);

(void) $paused->eval('host.sleepPaused()', '=pauses');

// Nothing is measured while paused, so nothing accumulates. That is the whole
// design: it is why none of the reference implementation's "the limit expired
// while we were not looking" reconstruction is needed here.
var_dump($observed['while paused'] === 0.0);

// And the same sleep with the timer running does accumulate. Asserted against
// half the sleep rather than the sleep itself, so a slow runner cannot flake
// it -- the assertion is "time passed", not "this much time passed".
var_dump($observed['while running'] >= 0.1);

// The call slept for 200 ms with the timer running, so wall time is now
// unambiguously ahead of CPU time -- the thread spent none. This is the
// assertion that fails if the two counters are ever read from the same clock by
// mistake, or if one forgets to close its segment, and unlike a busy loop a
// sleep leaves no room for the two to sit within measurement noise of each
// other.
var_dump($paused->stats()->wallClockSeconds > $paused->stats()->cpuSeconds + 0.1);

$paused->close();

?>
--EXPECT--
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
