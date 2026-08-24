--TEST--
Slots survive being handed out, torn down and handed out again under the watchdog
--EXTENSIONS--
luaext
--INI--
luaext.watchdog_resolution_us=100
--SKIPIF--
<?php
// The only --SKIPIF-- shape this suite allows, and it earns its place: every
// assertion below runs an unbounded loop to prove the CPU limit stops it. On a
// build where features() says the limit cannot be enforced at all, running an
// infinite loop to demonstrate that it is not enforced is pure waste -- and the
// harness would have to time each one out. The build that reports Unsupported
// is covered by tests/02-limits/hook-count-zero-voids-limits.phpt instead.
use DevelopGravity\LuaExt\LimitSupport;
use DevelopGravity\LuaExt\Sandbox;

if (Sandbox::features()['cpuLimit'] === LimitSupport::Unsupported) {
	echo "skip this build reports LimitSupport::Unsupported for the CPU limit";
}
?>
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Exception\CpuLimitError;
use DevelopGravity\LuaExt\Limits;
use DevelopGravity\LuaExt\Sandbox;
use DevelopGravity\LuaExt\SandboxConfig;

/*
 * The lifetime problem the watchdog is built around, exercised rather than
 * asserted about.
 *
 * The deadline heap holds RAW slot pointers, and a sandbox tearing down does
 * not take the watchdog's lock to remove its entries -- teardown happens on
 * every close and making it contend for a process-wide lock would be a real
 * bottleneck under a worker SAPI. So entries outlive the sandbox that published
 * them, and what makes that safe is two things together: the backing store is
 * never freed before MSHUTDOWN, and every slot carries a generation counter
 * that is bumped on release, so a stale entry is recognised and dropped when it
 * surfaces rather than followed.
 *
 * Neither of those is visible from PHP. What IS visible is whether hundreds of
 * short-lived sandboxes with live deadlines can be created and destroyed while
 * the watchdog thread is actively waking up on them. The INI above lowers the
 * wake-up floor so the thread runs hot for the whole test: a stale-pointer
 * follow or a missing generation check shows up here as a crash under ASan,
 * and a lock-order inversion as a hang.
 *
 * Two batches of limits: some slots are released while a deadline is still
 * pending in the heap, and some are released after the deadline has already
 * fired. Both are the interesting case, for opposite reasons.
 */

const ROUNDS = 400;

$stopped = 0;
$finished = 0;

for ($round = 0; $round < ROUNDS; $round++) {
	// Alternating so that roughly half the slots are torn down with an entry
	// still queued against them, and half after it has already been serviced.
	$expires = ($round % 2) === 0;

	$sandbox = new Sandbox(new SandboxConfig(
		limits: (new Limits())->with(
			cpuSeconds: $expires ? 0.001 : 5.0,
			wallClockSeconds: $expires ? 0.05 : 5.0,
		),
	));

	try {
		(void) $sandbox->eval('local x = 0 for i = 1, 1000000 do x = x + i end return x', '=churn');
		$finished++;
	} catch (CpuLimitError) {
		$stopped++;
	}

	// Closed explicitly rather than left to the destructor, so the release
	// happens at a point the test controls and while the watchdog is awake.
	$sandbox->close();
}

// Every round did one or the other, and neither outcome swallowed a round.
var_dump($stopped + $finished === ROUNDS);

// Both paths were actually taken. Without this the test could pass while
// exercising only half of what it is for.
var_dump($stopped > 0);
var_dump($finished > 0);

/*
 * Slots are reused rather than leaked, which is what keeps a long-lived worker
 * from walking the pool to exhaustion. Nothing exposes the pool, so this asks
 * the question the only way PHP can: after all that churn, a fresh sandbox
 * still gets a working slot and still enforces its limit.
 */
$last = new Sandbox(new SandboxConfig(
	limits: (new Limits())->with(cpuSeconds: 0.05, wallClockSeconds: 0.5),
));

try {
	(void) $last->eval('while true do end', '=runaway');
	echo "NOT STOPPED\n";
} catch (CpuLimitError) {
	echo "a slot handed out after the churn still enforces\n";
}

$last->close();

/*
 * Sandboxes alive at the same time, so the heap holds several entries at once
 * and the min-heap ordering is what decides which one the watchdog serves
 * first. Released in the reverse order they were created, so the heap sees its
 * entries invalidated out of deadline order.
 */
$live = [];

for ($index = 0; $index < 32; $index++) {
	$live[] = new Sandbox(new SandboxConfig(
		limits: (new Limits())->with(
			// Deliberately staggered, so no two share a deadline.
			cpuSeconds: 0.05 + ($index * 0.01),
			wallClockSeconds: 1.0 + ($index * 0.05),
		),
	));
}

foreach (array_reverse($live) as $sandbox) {
	$sandbox->close();
}

echo "done\n";

?>
--EXPECT--
bool(true)
bool(true)
bool(true)
a slot handed out after the churn still enforces
done
