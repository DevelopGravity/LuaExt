--TEST--
maxLiveCoroutines and maxCoroutineDepth bound what a script can create and nest
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

/*
 * Both caps are fatal rather than catchable, and that is the point: a script
 * able to pcall either one would retry in a loop, and both exist to bound what
 * the interpreter holds. A limit a script may decline is not a limit.
 *
 * The live cap collects before refusing. Most programs that reach it have simply
 * left finished coroutines lying around, and refusing those would make the limit
 * describe allocation history rather than what is actually alive.
 */

use DevelopGravity\LuaExt\Exception\CoroutineLimitError;
use DevelopGravity\LuaExt\Limits;
use DevelopGravity\LuaExt\Sandbox;
use DevelopGravity\LuaExt\SandboxConfig;

/* ---------------------------------------------------------------------------
 * Live count
 * ------------------------------------------------------------------------ */

$sandbox = new Sandbox(new SandboxConfig(
	limits: (new Limits())->with(maxLiveCoroutines: 8),
));

/*
 * All within ONE call, deliberately. Coroutines are call-scoped: the sweep at
 * the end of every outermost call closes them and the live count returns to
 * zero, so a cap spread across two evals would test nothing. That makes
 * maxLiveCoroutines a per-call ceiling, which is the only reading consistent
 * with the lifecycle guarantee.
 */
$made = $sandbox->eval(<<<'LUA'
	local kept = {}

	-- Exactly the cap, each suspended so none can be collected.
	for i = 1, 8 do
		kept[i] = coroutine.create(function() coroutine.yield() end)
		coroutine.resume(kept[i])
	end

	return #kept
LUA, '=atcap');

var_dump($made[0]);

try {
	(void) $sandbox->eval(<<<'LUA'
		local kept = {}

		for i = 1, 9 do
			kept[i] = coroutine.create(function() coroutine.yield() end)
			coroutine.resume(kept[i])
		end

		return #kept
	LUA, '=overcap');

	echo "NOT REFUSED\n";
} catch (CoroutineLimitError) {
	echo "refused past the live cap\n";
}

// A script cannot decline it.
try {
	(void) $sandbox->eval(<<<'LUA'
		local kept = {}

		local ok = pcall(function()
			for i = 1, 9 do
				kept[i] = coroutine.create(function() coroutine.yield() end)
				coroutine.resume(kept[i])
			end
		end)

		return ok
	LUA, '=swallow');

	echo "SWALLOWED\n";
} catch (CoroutineLimitError) {
	echo "pcall did not swallow the live cap\n";
}

$sandbox->close();

/* ---------------------------------------------------------------------------
 * Dead coroutines do not count against the cap
 * ------------------------------------------------------------------------ */

$recycling = new Sandbox(new SandboxConfig(
	limits: (new Limits())->with(maxLiveCoroutines: 4),
));

// Far more than the cap, but each finishes and becomes garbage before the next.
// The collection create() runs on hitting the cap is what makes this work.
$total = $recycling->eval(<<<'LUA'
	local n = 0

	for _ = 1, 200 do
		local co = coroutine.create(function() return 1 end)
		local _, value = coroutine.resume(co)
		n = n + value
	end

	return n
LUA, '=recycle');

var_dump($total[0]);

$recycling->close();

/* ---------------------------------------------------------------------------
 * Nesting depth
 * ------------------------------------------------------------------------ */

$nested = new Sandbox(new SandboxConfig(
	limits: (new Limits())->with(maxCoroutineDepth: 4),
));

try {
	(void) $nested->eval(<<<'LUA'
		-- Each level resumes another, so depth grows without the live count
		-- coming anywhere near its own default cap.
		local function deeper(level)
			local co = coroutine.create(function() deeper(level + 1) end)
			local ok, err = coroutine.resume(co)

			if not ok then error(err, 0) end
		end

		deeper(1)
	LUA, '=deep');

	echo "NOT REFUSED\n";
} catch (CoroutineLimitError) {
	echo "refused past the depth cap\n";
}

$nested->close();

echo "survived\n";

?>
--EXPECT--
int(8)
refused past the live cap
pcall did not swallow the live cap
int(200)
refused past the depth cap
survived
