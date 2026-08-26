--TEST--
No suspended coroutine survives the call that created it, and closing one is metered
--EXTENSIONS--
luaext
--SKIPIF--
<?php
// See tests/02-limits/cpu-limit-not-refunded.phpt for why this shape of skip is
// allowed: the last assertion runs an unbounded loop to prove the limit stops
// it, which is pure waste on a build that reports the limit as Unsupported.
use DevelopGravity\LuaExt\LimitSupport;
use DevelopGravity\LuaExt\Sandbox;

if (Sandbox::features()['cpuLimit'] === LimitSupport::Unsupported) {
	echo "skip this build reports LimitSupport::Unsupported for the CPU limit";
}
?>
--FILE--
<?php

declare(strict_types=1);

/*
 * The core coroutine guarantee: when the outermost call returns, no suspended
 * Lua execution state is left anywhere. A script can stash a coroutine in a
 * global, but what it finds there next time is a dead thread, not a paused one.
 *
 * The third case is the one with teeth. Force-closing runs <close> variables,
 * which is untrusted Lua, so WHERE the sweep happens is a security property: it
 * runs inside the timing bracket, before the boundary disarms the watchdog and
 * clears the sticky interrupt flag. A sweep after that point would run these
 * handlers unmetered and uninterruptible, and `while true do end` in one would
 * hang the process.
 */

use DevelopGravity\LuaExt\Exception\CpuLimitError;
use DevelopGravity\LuaExt\Limits;
use DevelopGravity\LuaExt\Sandbox;
use DevelopGravity\LuaExt\SandboxConfig;

/* ---------------------------------------------------------------------------
 * A stashed coroutine is dead in the next call
 * ------------------------------------------------------------------------ */

$sandbox = new Sandbox();

(void) $sandbox->eval(<<<'LUA'
	kept = coroutine.create(function()
		coroutine.yield("first")
		return "second"
	end)

	-- Suspended, mid-body, with more to do.
	local ok, value = coroutine.resume(kept)
	assert(ok and value == "first", "the first resume should yield")
LUA, '=stash');

// A different call. The thread object survives as a value; its execution state
// does not.
$status = $sandbox->eval('return coroutine.status(kept)', '=check');
var_dump($status[0]);

$resumed = $sandbox->eval('return select(1, coroutine.resume(kept))', '=revive');
var_dump($resumed[0]);

$sandbox->close();

/* ---------------------------------------------------------------------------
 * <close> variables run during the sweep
 * ------------------------------------------------------------------------ */

$sandbox = new Sandbox();

(void) $sandbox->eval(<<<'LUA'
	closed = false

	leaked = coroutine.create(function()
		local guard <close> = setmetatable({}, {__close = function() closed = true end})
		coroutine.yield()
		return "never reached"
	end)

	coroutine.resume(leaked)
	-- Suspended, holding a to-be-closed variable, and never resumed again.
LUA, '=leak');

// The sweep closed it, which ran the handler.
$closed = $sandbox->eval('return closed', '=closed');
var_dump($closed[0]);

$sandbox->close();

/* ---------------------------------------------------------------------------
 * A <close> handler that will not stop is stopped anyway
 * ------------------------------------------------------------------------ */

$bounded = new Sandbox(new SandboxConfig(
	limits: (new Limits())->with(cpuSeconds: 0.10, wallClockSeconds: 2.0),
));

try {
	(void) $bounded->eval(<<<'LUA'
		hostile = coroutine.create(function()
			local guard <close> = setmetatable({}, {__close = function()
				while true do end
			end})
			coroutine.yield()
		end)

		coroutine.resume(hostile)
	LUA, '=hostile');

	echo "NOT STOPPED\n";
} catch (CpuLimitError) {
	// The sweep ran the handler under the remaining budget and the limit caught
	// it. Unmetered, this call would never have returned.
	echo "the close handler was metered\n";
}

$bounded->close();

echo "survived\n";

?>
--EXPECT--
string(4) "dead"
bool(false)
bool(true)
the close handler was metered
survived
