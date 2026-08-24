--TEST--
A CPU limit too fine for the platform clock still stops the script, and still says CPU
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Exception\CpuLimitError;
use DevelopGravity\LuaExt\Exception\WallClockLimitError;
use DevelopGravity\LuaExt\Sandbox;

/*
 * Whether a PARTICULAR limit can be measured is not a platform question, which
 * is why it is not features()' to answer: features() is static and has no limit
 * in front of it. A limit within a few ticks of the clock's own resolution is
 * unmeasurable on any platform, including the ones features() calls Enforced.
 *
 * When that happens the limit is DEGRADED: a wall-clock companion deadline is
 * armed so the script still stops. What it must not do is report
 * WallClockLimitError. The host asked for a CPU limit and was told the platform
 * is coarse; answering with the name of a limit it never set would send it
 * looking for a wallClockSeconds it does not have.
 *
 * The limit is derived from the reported resolution rather than hardcoded, so
 * this is degraded on a 1 ns Linux clock, a 1 us macOS clock and a 15.6 ms
 * Windows tick alike.
 */

$resolution = Sandbox::features()['cpuResolutionSeconds'];

var_dump($resolution > 0.0);

// Ten ticks. Comfortably under the twenty the implementation requires before it
// will call a limit measurable, on every platform, without naming any of them.
$unmeasurable = $resolution * 10;

$sandbox = new Sandbox();

// No wall-clock limit at all, so nothing this test observes can come from one
// the host set: whatever stops the script below is the degraded companion.
$sandbox->setWallClockLimit(null);
$sandbox->setCpuLimit($unmeasurable);

try {
	(void) $sandbox->eval('while true do end', '=runaway');
	echo "NOT STOPPED\n";
} catch (WallClockLimitError) {
	echo "WRONG CLASS: reported the wall clock for a CPU limit\n";
} catch (CpuLimitError) {
	echo "stopped, reported as a CPU limit\n";
} catch (Throwable $error) {
	printf("WRONG CLASS %s\n", $error::class);
}

$sandbox->close();

// A limit far above the resolution is measurable, is not degraded, and reports
// the same class -- so the two paths are indistinguishable from outside, which
// is the whole point of degrading rather than refusing.
$measurable = new Sandbox();
$measurable->setWallClockLimit(null);
$measurable->setCpuLimit(max(0.05, $resolution * 1000));

try {
	(void) $measurable->eval('while true do end', '=runaway');
	echo "NOT STOPPED\n";
} catch (CpuLimitError) {
	echo "stopped, reported as a CPU limit\n";
} catch (Throwable $error) {
	printf("WRONG CLASS %s\n", $error::class);
}

$measurable->close();

?>
--EXPECT--
bool(true)
stopped, reported as a CPU limit
stopped, reported as a CPU limit
