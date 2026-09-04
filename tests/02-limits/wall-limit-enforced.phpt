--TEST--
A script blocked outside the interpreter is stopped by its wall-clock limit
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Exception\WallClockLimitError;
use DevelopGravity\LuaExt\LimitSupport;
use DevelopGravity\LuaExt\Limits;
use DevelopGravity\LuaExt\Sandbox;
use DevelopGravity\LuaExt\SandboxConfig;

/*
 * The wall-clock limit exists for the case the CPU limit cannot see: a host
 * callback that is not spending CPU at all, because it is asleep on a socket or
 * a slow filesystem backend. No amount of in-VM checking finds that, since no
 * VM instruction executes while it happens -- which is the one thing the
 * watchdog thread is strictly required for.
 *
 * No elapsed time is asserted. The generous margins below (a 0.2 s limit
 * against a 3 s sleep) exist so that a loaded CI runner cannot turn a real trip
 * into a flake, not so that a duration can be checked.
 */

const WALL_SECONDS = 0.2;
const SLEEP_SECONDS = 3;

// Without the watchdog thread this limit degrades to "trips when Lua next runs
// an instruction", which would make the sleeping-callback case below unenforced.
// features() has to say so rather than conceal it.
var_dump(Sandbox::features()['wallClockLimit'] === LimitSupport::Enforced);

$sandbox = new Sandbox(new SandboxConfig(
	limits: (new Limits())->with(
		// No CPU limit at all, so nothing but the wall clock can stop this.
		cpuSeconds: null,
		wallClockSeconds: WALL_SECONDS,
	),
));

$sandbox->registerLibrary('host', [
	// usleep, not a busy loop: the point is that the owning thread consumes no
	// CPU whatsoever while the limit runs out.
	'sleep' => static function (): void {
		usleep((int) (SLEEP_SECONDS * 1_000_000));
	},
]);

try {
	(void) $sandbox->eval('host.sleep() return "finished"', '=sleeper');
	echo "NOT STOPPED\n";
} catch (WallClockLimitError) {
	echo "sleeping callback stopped\n";
} catch (Throwable $error) {
	printf("WRONG CLASS %s\n", $error::class);
}

// Wall time really was billed, and it is a different quantity from CPU time --
// the callback slept, so the CPU number must stay near zero while the wall
// number passes the limit. This is the one assertion that would still pass if
// the two counters were accidentally the same variable, so both halves matter.
var_dump($sandbox->stats()->wallClockSeconds >= WALL_SECONDS);
var_dump($sandbox->stats()->cpuSeconds < WALL_SECONDS);

$sandbox->close();

// A CPU-bound script also trips the wall limit, because CPU time is wall time
// too. This is the case where either limit could fire; the class says which
// budget was actually exhausted rather than which mechanism noticed.
$spinner = new Sandbox(new SandboxConfig(
	limits: (new Limits())->with(
		// An order of magnitude above the wall limit, so the wall limit is what
		// fires -- and so a wall-limit regression fails in two seconds with the
		// wrong class instead of spinning until the harness gives up.
		cpuSeconds: WALL_SECONDS * 10,
		wallClockSeconds: WALL_SECONDS,
	),
));

try {
	(void) $spinner->eval('while true do end', '=spinner');
	echo "NOT STOPPED\n";
} catch (WallClockLimitError) {
	echo "busy loop stopped\n";
} catch (Throwable $error) {
	printf("WRONG CLASS %s\n", $error::class);
}

$spinner->close();

?>
--EXPECT--
bool(true)
sleeping callback stopped
bool(true)
bool(true)
busy loop stopped
