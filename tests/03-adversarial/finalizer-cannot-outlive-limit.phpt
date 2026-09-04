--TEST--
A __gc finaliser cannot outlive the CPU limit, and cannot hide that it tried
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
 * The one place in Lua where a sandbox's guarantees fail twice over, and the
 * reason this has a file of its own rather than a row in a table.
 *
 * lgc.c's GCTM does two things to a finaliser. It clears L->allowhook for the
 * duration, so no hook can fire inside one -- and the count hook is the only
 * mechanism that can interrupt the interpreter's dispatch loop. Then it hands
 * any error the finaliser raised to luaE_warnerror and carries on, so even an
 * error that did get raised goes nowhere.
 *
 * Together those make
 *
 *     setmetatable({}, {__gc = function() while true do end end})
 *
 * both unstoppable and unreportable on stock Lua. Two vendored patches fix it:
 * the hook stays armed when it is OURS (a C function that allocates nothing and
 * either returns or raises, so upstream's reason for disabling hooks does not
 * apply), and a finaliser error propagates instead of warning when the atomic
 * interrupt flag is raised.
 *
 * A finaliser error unwinding through a collector step is a path upstream never
 * exercises. That is why this test also checks that the sandbox survives it.
 */

const CPU_SECONDS = 0.05;
const WALL_BACKSTOP_SECONDS = 0.5;

function bounded(): Sandbox
{
	return new Sandbox(new SandboxConfig(
		limits: (new Limits())->with(
			cpuSeconds: CPU_SECONDS,
			// A backstop an order of magnitude above the CPU limit. If the
			// finaliser ever becomes unstoppable again this fails in half a
			// second with the wrong exception class rather than hanging CI
			// forever, which is how it used to fail.
			wallClockSeconds: WALL_BACKSTOP_SECONDS,
		),
	));
}

function stopped(string $label, string $code): void
{
	$sandbox = bounded();

	try {
		(void) $sandbox->eval($code, '=finaliser');
		printf("%-28s NOT STOPPED\n", $label);
	} catch (CpuLimitError) {
		printf("%-28s stopped\n", $label);
	} catch (Throwable $error) {
		printf("%-28s WRONG CLASS %s\n", $label, $error::class);
	}

	$sandbox->close();
}

// Collected explicitly, which is the shortest path to the finaliser.
stopped('explicit collection', <<<'LUA'
	setmetatable({}, {__gc = function() while true do end end})

	collectgarbage("collect")

	return "swallowed"
LUA);

// Collected by the incremental collector during ordinary allocation, so the
// finaliser runs from inside a GC step rather than from a collectgarbage call.
// This is the path where the error unwinds through the collector's own loop.
stopped('incidental collection', <<<'LUA'
	for i = 1, 200 do
		setmetatable({}, {__gc = function() while true do end end})
	end

	local t = {}

	for i = 1, 2000000 do
		t[1] = {i}
	end

	return "swallowed"
LUA);

// A finaliser that catches its own interruption and keeps going. pcall inside
// a finaliser inside the collector is three protected calls deep, and the
// interrupt flag is sticky through all of them.
stopped('pcall inside the finaliser', <<<'LUA'
	setmetatable({}, {__gc = function()
		pcall(function() while true do end end)

		while true do end
	end})

	collectgarbage("collect")

	return "swallowed"
LUA);

// A finaliser that raises an ordinary error is NOT a limit breach, and must
// keep upstream's behaviour exactly: warned about, swallowed, script carries
// on. The patch keys on the atomic interrupt flag precisely so that it can tell
// these apart without inspecting the error value -- which would mean Lua stack
// calls that can allocate, inside the collector.
$ordinary = bounded();

var_dump($ordinary->eval(<<<'LUA'
	setmetatable({}, {__gc = function() error("just a bug") end})

	collectgarbage("collect")

	return "carried on"
LUA, '=ordinary'));

$ordinary->close();

/*
 * The sandbox survives an interrupted finaliser. The throw abandons the rest of
 * the collector step, so the objects still awaiting finalisation stay pending;
 * closing has to run them without tripping over the state the unwind left.
 */
$survivor = bounded();

try {
	(void) $survivor->eval(<<<'LUA'
		for i = 1, 50 do
			setmetatable({}, {__gc = function() while true do end end})
		end

		collectgarbage("collect")
	LUA, '=survivor');
} catch (CpuLimitError) {
	echo "stopped with finalisers still pending\n";
}

var_dump($survivor->stats()->memoryBytes > 0);

// Closes cleanly, and in bounded time. Teardown deliberately runs with the
// interrupt RAISED: lua_close() runs every pending finaliser with nothing left
// measuring anything, so leaving the flag down here is what used to make close()
// hang forever on exactly the objects this test creates.
$survivor->close();
var_dump($survivor->isClosed());

?>
--EXPECT--
explicit collection          stopped
incidental collection        stopped
pcall inside the finaliser   stopped
array(1) {
  [0]=>
  string(10) "carried on"
}
stopped with finalisers still pending
bool(true)
bool(true)
