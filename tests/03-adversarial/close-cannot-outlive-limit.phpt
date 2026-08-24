--TEST--
A <close> handler cannot outlive the CPU limit, and needed no patch to say so
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Exception\CpuLimitError;
use DevelopGravity\LuaExt\Limits;
use DevelopGravity\LuaExt\Sandbox;
use DevelopGravity\LuaExt\SandboxConfig;

/*
 * The sibling of finalizer-cannot-outlive-limit.phpt, and the point of it is
 * the ASYMMETRY between the two rather than the result they share.
 *
 * A __gc metamethod runs from lgc.c's GCTM, which clears L->allowhook for the
 * duration and turns any error into a warning. Both had to be patched in the
 * vendored tree before a finaliser could be stopped or could report having been
 * stopped.
 *
 * A <close> handler runs from lfunc.c's prepcallclosemth, which touches
 * L->allowhook not at all and lets the error propagate the way any other error
 * does. So the count hook is already live inside one and the fatal already
 * escapes -- no patch, and none wanted.
 *
 * This test exists so the asymmetry is pinned rather than remembered. If a
 * future Lua release makes to-be-closed variables look more like finalisers,
 * this fails and the reason is written down right here.
 */

const CPU_SECONDS = 0.05;
const WALL_BACKSTOP_SECONDS = 0.5;

function bounded(): Sandbox
{
	return new Sandbox(new SandboxConfig(
		limits: (new Limits())->with(
			cpuSeconds: CPU_SECONDS,
			// A backstop an order of magnitude above the CPU limit, so a
			// regression here fails in half a second rather than hanging CI.
			wallClockSeconds: WALL_BACKSTOP_SECONDS,
		),
	));
}

function stopped(string $label, string $code): void
{
	$sandbox = bounded();

	try {
		(void) $sandbox->eval($code, '=closer');
		printf("%-28s NOT STOPPED\n", $label);
	} catch (CpuLimitError) {
		printf("%-28s stopped\n", $label);
	} catch (Throwable $error) {
		printf("%-28s WRONG CLASS %s\n", $label, $error::class);
	}

	$sandbox->close();
}

// The plain case: the handler runs at the end of the block and never returns.
stopped('loops in the handler', <<<'LUA'
	do
		local _ <close> = setmetatable({}, {__close = function() while true do end end})
	end

	return "swallowed"
LUA);

// Closed by an error rather than by falling off the end of the block, so the
// handler runs while an error is already travelling. Lua has to close the
// variable before the error can continue, and the CPU limit has to win anyway.
stopped('runs during an unwind', <<<'LUA'
	local ok = pcall(function()
		local _ <close> = setmetatable({}, {__close = function() while true do end end})

		error("something else went wrong")
	end)

	return "swallowed"
LUA);

// A handler that catches its own interruption. The interrupt flag is sticky, so
// the next check raises again -- and again -- however many pcalls are nested
// between the loop and the boundary.
stopped('pcall inside the handler', <<<'LUA'
	do
		local _ <close> = setmetatable({}, {__close = function()
			pcall(function() while true do end end)

			while true do end
		end})
	end

	return "swallowed"
LUA);

// The two mechanisms together, since a to-be-closed variable holding an object
// with a finaliser is the shape a real resource wrapper takes.
stopped('with a finaliser too', <<<'LUA'
	do
		local _ <close> = setmetatable({}, {
			__close = function() while true do end end,
			__gc = function() while true do end end,
		})
	end

	return "swallowed"
LUA);

/*
 * A <close> handler that raises an ordinary error is NOT swallowed, unlike the
 * same error in a __gc metamethod. That is upstream behaviour and this build
 * does not change it: the error becomes the block's error and reaches the host.
 */
$ordinary = bounded();

try {
	(void) $ordinary->eval(<<<'LUA'
		do
			local _ <close> = setmetatable({}, {__close = function() error("from the handler") end})
		end

		return "carried on"
	LUA, '=ordinary');

	echo "SWALLOWED\n";
} catch (CpuLimitError) {
	echo "WRONG CLASS\n";
} catch (Throwable $error) {
	printf("ordinary error escapes: %s\n", $error::class);
}

$ordinary->close();

?>
--EXPECT--
loops in the handler         stopped
runs during an unwind        stopped
pcall inside the handler     stopped
with a finaliser too         stopped
ordinary error escapes: DevelopGravity\LuaExt\Exception\RuntimeError
