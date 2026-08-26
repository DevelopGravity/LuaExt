--TEST--
The wrapped coroutine library behaves like Lua's, including yielding across pcall
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

/*
 * The wrapper exists to add caps and to stop resume swallowing a fatal. It must
 * not quietly change the language while it is there -- a sandbox that runs
 * ordinary Lua differently is a worse tool even when it is a safer one.
 *
 * The pcall case is the one most easily broken by accident: ours uses lua_pcallk
 * with a continuation precisely so a coroutine can still yield across it, and
 * dropping that would be a language change smuggled in as a security fix.
 */

use DevelopGravity\LuaExt\Sandbox;

$sandbox = new Sandbox();

$results = $sandbox->eval(<<<'LUA'
	local out = {}
	local function record(...) out[#out + 1] = table.concat({...}, "|") end

	-- Values in both directions, across two suspensions.
	local co = coroutine.create(function(a, b)
		local c = coroutine.yield(a + b)
		local d = coroutine.yield(c * 2)
		return a, d
	end)

	record(select(2, coroutine.resume(co, 1, 2)))   -- 3
	record(select(2, coroutine.resume(co, 10)))     -- 20
	record(select(2, coroutine.resume(co, "end")))  -- 1|end
	record(coroutine.status(co))                    -- dead

	-- wrap propagates instead of returning ok/err.
	local gen = coroutine.wrap(function()
		for i = 1, 3 do coroutine.yield(i) end
	end)
	record(gen(), gen(), gen())

	-- A runtime error inside a coroutine is catchable: it is the script's own
	-- mistake, not a limit.
	local bad = coroutine.create(function() error("boom", 0) end)
	local bad_ok, bad_err = coroutine.resume(bad)
	record(tostring(bad_ok), bad_err)

	-- Yielding across pcall still works.
	local across = coroutine.create(function()
		local ok, value = pcall(function() return coroutine.yield("inside") end)
		return tostring(ok), value
	end)
	record(select(2, coroutine.resume(across)))
	record(select(2, coroutine.resume(across, "resumed")))

	-- running() distinguishes the main thread.
	local _, is_main = coroutine.running()
	record(tostring(is_main))

	local inner = coroutine.create(function()
		local _, main = coroutine.running()
		coroutine.yield(tostring(main))
	end)
	record(select(2, coroutine.resume(inner)))

	-- isyieldable is false on the main thread, true inside a coroutine.
	record(tostring(coroutine.isyieldable()))

	local ask = coroutine.create(function() coroutine.yield(tostring(coroutine.isyieldable())) end)
	record(select(2, coroutine.resume(ask)))

	-- close() on a suspended coroutine reports success and kills it.
	local closing = coroutine.create(function() coroutine.yield() end)
	coroutine.resume(closing)
	record(tostring(coroutine.close(closing)), coroutine.status(closing))

	return table.concat(out, "\n")
LUA, '=semantics');

echo $results[0], "\n";

/* ---------------------------------------------------------------------------
 * No yielding across the PHP boundary
 * ------------------------------------------------------------------------ */

$sandbox->registerLibrary('host', [
	// Calls back into Lua, so a yield underneath it would have to cross a PHP
	// frame. Lua raises rather than allowing it; this pins that it still does.
	'call' => static function () use ($sandbox): void {
		(void) $sandbox->call('tries_to_yield');
	},
]);

$crossed = $sandbox->eval(<<<'LUA'
	function tries_to_yield()
		coroutine.yield("nope")
	end

	local co = coroutine.create(function() host.call() end)
	local ok, err = coroutine.resume(co)

	return tostring(ok), tostring(err):match("C%-call boundary") ~= nil
LUA, '=boundary');

var_dump($crossed[0], $crossed[1]);

$sandbox->close();

?>
--EXPECT--
3
20
1|end
dead
1|2|3
false|boom
inside
true|resumed
true
false
false
true
true|dead
string(5) "false"
bool(true)
