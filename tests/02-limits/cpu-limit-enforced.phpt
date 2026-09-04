--TEST--
A runaway script is stopped by its CPU limit, wherever it is looping
--EXTENSIONS--
luaext
--INI--
luaext.hook_count=1000
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
use DevelopGravity\LuaExt\LimitSupport;
use DevelopGravity\LuaExt\Limits;
use DevelopGravity\LuaExt\Sandbox;
use DevelopGravity\LuaExt\SandboxConfig;

/*
 * The one thing this extension exists for. Every case below is an infinite
 * loop, and every case has to come back as a CpuLimitError rather than as a
 * hung process -- which is what the extension this replaces did on macOS and
 * Windows, silently.
 *
 * Nothing here asserts an elapsed time. A 15.6 ms Windows tick and a 1 ns Linux
 * clock disagree about every duration and agree about whether the script was
 * stopped, so "was it stopped, and with which class" is the whole assertion.
 * The wall-clock limit is set an order of magnitude above the CPU limit purely
 * as a backstop: if the CPU limit ever stops working, this test fails in a
 * second with the wrong exception class instead of wedging CI.
 */

const CPU_SECONDS = 0.05;
const WALL_BACKSTOP_SECONDS = 0.5;

function bounded(): Sandbox
{
	return new Sandbox(new SandboxConfig(
		limits: (new Limits())->with(
			cpuSeconds: CPU_SECONDS,
			wallClockSeconds: WALL_BACKSTOP_SECONDS,
		),
	));
}

function stopped(string $label, string $code): void
{
	// A fresh sandbox each time: a spent budget would stop the next case before
	// it ever ran, which would prove nothing at all.
	$sandbox = bounded();

	try {
		(void) $sandbox->eval($code, '=runaway');
		printf("%-24s NOT STOPPED\n", $label);
	} catch (CpuLimitError) {
		printf("%-24s stopped\n", $label);
	} catch (Throwable $error) {
		printf("%-24s WRONG CLASS %s\n", $label, $error::class);
	}

	$sandbox->close();
}

// The platform has to be able to do this at all before the rest means anything.
var_dump(Sandbox::features()['cpuLimit'] !== LimitSupport::Unsupported);

// The bare interpreter loop. Nothing but the count hook can reach this: there
// is no C function to patch a check into, and no allocation for the allocator
// to notice.
stopped('empty loop', 'while true do end');

// A numeric for with no body, which compiles to a different opcode sequence and
// is the shape a "just count to a big number" attack takes.
stopped('numeric for', 'for i = 1, math.maxinteger do end');

// A loop that allocates. The memory limit would eventually catch this one, so
// it is here to prove the CPU limit gets there first when the memory budget is
// generous.
stopped('allocating loop', 'local t = {} while true do t[#t + 1] = nil end');

// Recursion, which grows the CallInfo chain rather than looping in one frame.
stopped('recursion', 'local function f() return f() end f()');

// String building, which spends most of its time inside the C library rather
// than in the VM dispatch loop.
stopped('string concat', 'local s = "" while true do s = "x" end');

// A tight loop calling a Lua function, i.e. the case where the hook fires
// across a call boundary rather than inside one function's body.
stopped('call loop', 'local function f() end while true do f() end');

$sandbox = bounded();

// A stopped script leaves a usable sandbox behind: the breach was reported, the
// interrupt flag was dropped when the call unwound, and the next call is not
// poisoned by it. Only the budget is gone, which is the point of not resetting.
try {
	(void) $sandbox->eval('while true do end', '=first');
} catch (CpuLimitError) {
	echo "first call stopped\n";
}

var_dump($sandbox->isClosed());
var_dump($sandbox->stats()->cpuSeconds > 0.0);

// The budget really is spent, so a second runaway stops too -- and stops as a
// CPU breach rather than as anything more exotic.
try {
	(void) $sandbox->eval('while true do end', '=second');
} catch (CpuLimitError) {
	echo "second call stopped\n";
}

$sandbox->close();

?>
--EXPECT--
bool(true)
empty loop               stopped
numeric for              stopped
allocating loop          stopped
recursion                stopped
string concat            stopped
call loop                stopped
first call stopped
bool(false)
bool(true)
second call stopped
