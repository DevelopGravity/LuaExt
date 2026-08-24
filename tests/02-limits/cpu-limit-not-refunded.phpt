--TEST--
Setting a CPU limit again does not refund what has already been spent
--EXTENSIONS--
luaext
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
 * A deliberate divergence from the extension this replaces, and the reason for
 * it fits in one sentence: its setCPULimit() reset the consumed counter, so a
 * host callback could call it in a loop and the script would never stop.
 *
 * Here the new budget is limit-minus-used. Setting the same limit again buys
 * nothing, and setting a smaller one than has already been spent stops the
 * script at the next opportunity rather than at some point in the future.
 *
 * It also keeps getCpuUsage() and the limit describing the same quantity, which
 * is what makes them comparable at all.
 */

const CPU_SECONDS = 0.20;
const WALL_BACKSTOP_SECONDS = 2.0;

/*
 * How far over budget the script may run before this counts as unenforced.
 *
 * Generous on purpose -- a loaded runner is allowed to be slow, and the row
 * below deliberately asserts nothing about elapsed WALL time. What it catches is
 * a limit that is technically honoured but arbitrarily late: publishing a new
 * deadline used not to re-evaluate the budget, so a callback resetting the limit
 * invalidated the watchdog's pending entry faster than the thread could act on
 * it, and this loop ran ~50x over budget -- straight through the wall backstop
 * too -- before anything stopped it. Ten times the limit is far outside anything
 * scheduling noise produces and far inside that failure.
 */
const OVERSHOOT_ALLOWED = 10.0;

$sandbox = new Sandbox(new SandboxConfig(
	limits: (new Limits())->with(
		cpuSeconds: CPU_SECONDS,
		wallClockSeconds: WALL_BACKSTOP_SECONDS,
	),
));

$attempts = 0;

$sandbox->registerLibrary('host', [
	// The attack: reset the budget from inside the sandbox's own execution and
	// keep going. Under the reference's semantics this never returns.
	'extend' => static function () use ($sandbox, &$attempts): void {
		$attempts++;
		$sandbox->setCpuLimit(CPU_SECONDS);
	},
]);

try {
	(void) $sandbox->eval(<<<'LUA'
		-- EVERY iteration, which is the adversarial case rather than a
		-- representative one. Each call republishes the sandbox's watchdog
		-- deadline, so calling as fast as the boundary allows is exactly how a
		-- script would starve a watchdog that only ever decided on its own
		-- schedule. A slower cadence lets the thread win the race on a fast
		-- machine and the test passes without proving anything.
		while true do
			host.extend()
		end
	LUA, '=extender');

	echo "NOT STOPPED\n";
} catch (CpuLimitError) {
	echo "stopped despite resetting the limit\n";
}

// The callback really did run: this test would pass trivially if the loop had
// stopped before ever reaching it.
var_dump($attempts > 0);

// Usage is cumulative across calls too, for the same reason. A sandbox that
// reset on every entry would let a host run unbounded work one call at a time.
var_dump($sandbox->getCpuUsage() >= CPU_SECONDS);

// And it stopped anywhere near the budget rather than eventually. See
// OVERSHOOT_ALLOWED: "it does stop, given long enough" is not enforcement.
var_dump($sandbox->getCpuUsage() < CPU_SECONDS * OVERSHOOT_ALLOWED);

$sandbox->close();

// Raising the limit does give a script more room -- the budget is
// limit-minus-used, so a bigger limit is a bigger budget. What it cannot do is
// un-spend anything.
$second = new Sandbox(new SandboxConfig(
	limits: (new Limits())->with(cpuSeconds: 0.05, wallClockSeconds: WALL_BACKSTOP_SECONDS),
));

try {
	(void) $second->eval('while true do end', '=first');
} catch (CpuLimitError) {
	echo "small budget spent\n";
}

$spent = $second->getCpuUsage();

// Below what has already been used, so there is no budget left at all and the
// next call stops without doing any work.
$second->setCpuLimit($spent / 2);

try {
	(void) $second->eval('return 1', '=nothing');
	echo "RAN ANYWAY\n";
} catch (CpuLimitError) {
	echo "a limit below what is spent leaves no budget\n";
}

$second->close();

?>
--EXPECT--
stopped despite resetting the limit
bool(true)
bool(true)
bool(true)
small budget spent
a limit below what is spent leaves no budget
