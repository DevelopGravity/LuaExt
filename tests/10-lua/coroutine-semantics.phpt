--TEST--
Lua conformance: coroutine semantics, which the older extension removed entirely
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

require __DIR__ . '/conformance.inc';

// luasandbox removed coroutines because its timeout hook could not span them.
// This build keeps them and follows the interrupt into whichever coroutine is
// running, and it replaces resume and wrap so a fatal cannot be swallowed --
// tests/03-adversarial/ covers that. What is checked here is that the ORDINARY
// semantics survived being wrapped: values across a yield, status transitions,
// and errors coming back as `false, message` rather than propagating.

conformance(<<<'LUA'
	-- Values pass both ways: resume's extra arguments become yield's results,
	-- and yield's arguments become resume's results.
	local echo = coroutine.create(function (a, b)
		local c, d = coroutine.yield(a + b)
		return c * d
	end)
	row('first resume', coroutine.resume(echo, 2, 3))
	row('second resume', coroutine.resume(echo, 4, 5))
	row('resume dead', coroutine.resume(echo))

	-- status through the whole lifecycle, including 'running' seen from inside
	-- and 'normal' for a coroutine that resumed another.
	local inner, outer
	inner = coroutine.create(function ()
		coroutine.yield(coroutine.status(inner), coroutine.status(outer))
	end)
	outer = coroutine.create(function ()
		coroutine.resume(inner)
		coroutine.yield(coroutine.status(outer))
	end)
	row('status fresh', coroutine.status(outer))
	local _, outer_status = coroutine.resume(outer)
	row('status of self', outer_status)
	row('status suspended', coroutine.status(outer), coroutine.status(inner))
	coroutine.resume(outer)
	row('status dead', coroutine.status(outer))

	-- running reports the coroutine and whether it is the main one.
	local main, is_main = coroutine.running()
	row('running in main', type(main), is_main)
	local co = coroutine.create(function ()
		local self, main_flag = coroutine.running()
		coroutine.yield(type(self), main_flag)
	end)
	row('running inside', select(2, coroutine.resume(co)))

	-- isyieldable is false in the main coroutine and true inside one.
	row('isyieldable main', coroutine.isyieldable())
	local yieldable = coroutine.create(function () coroutine.yield(coroutine.isyieldable()) end)
	row('isyieldable inside', select(2, coroutine.resume(yieldable)))

	-- An ordinary error inside a coroutine comes back as false plus the message.
	-- It does NOT propagate to the resumer, which is what makes resume a pcall
	-- in disguise.
	local failing = coroutine.create(function () error('inside', 0) end)
	row('error via resume', coroutine.resume(failing))
	row('status after error', coroutine.status(failing))

	-- wrap propagates instead, and the propagated error is catchable.
	local wrapped = coroutine.wrap(function () error('wrapped', 0) end)
	try('error via wrap', wrapped)

	-- wrap's happy path is a plain function returning the yielded values.
	local counter = coroutine.wrap(function ()
		for index = 1, 3 do coroutine.yield(index) end
		return 'done'
	end)
	row('wrap', counter(), counter(), counter(), counter())

	-- Resuming a dead or running coroutine is refused rather than undefined.
	local dead = coroutine.create(function () end)
	coroutine.resume(dead)
	row('resume dead again', coroutine.resume(dead))

	-- RESUMING ITSELF. The refusal must arrive as `false, message` like every
	-- other refusal -- and it did not, until this file was written: resume moved
	-- the error across with lua_xmove, which is a no-op when the source and
	-- destination are the same stack, so the false landed on top of the message
	-- and the pair came back reversed. Only reachable when a coroutine resumes
	-- itself, which is why nothing else caught it.
	local selfresume
	selfresume = coroutine.create(function ()
		local ok, err = coroutine.resume(selfresume)
		return ok, err
	end)
	row('resume self', select(2, coroutine.resume(selfresume)))

	-- CLOSING A COROUTINE THAT IS NOT SUSPENDED OR DEAD. Closing resets the
	-- thread's stack, so doing it to one that is still executing resets the
	-- stack the current frame is running on. Upstream refuses both cases; this
	-- build did not until the same pass that found the resume bug above.
	local selfclose
	selfclose = coroutine.create(function () return pcall(coroutine.close, selfclose) end)
	row('close running', select(2, coroutine.resume(selfclose)))

	local nested_inner, nested_outer
	nested_inner = coroutine.create(function () return pcall(coroutine.close, nested_outer) end)
	nested_outer = coroutine.create(function ()
		return select(2, coroutine.resume(nested_inner))
	end)
	row('close normal', select(2, coroutine.resume(nested_outer)))

	-- close moves a suspended coroutine to dead and runs its to-be-closed
	-- variables; closing a dead one is a no-op that still reports success.
	local closed = {}
	local closable = coroutine.create(function ()
		local guard <close> = setmetatable({}, {__close = function () closed[#closed + 1] = 'closed' end})
		coroutine.yield('yielded')
	end)
	coroutine.resume(closable)
	row('close', coroutine.close(closable), coroutine.status(closable), closed)
	row('close again', coroutine.close(closable))

	-- A yield with no coroutine to yield from is an error, not a silent return.
	try('yield in main', coroutine.yield, 1)

	-- coroutine.create needs a function, not a callable table.
	try('create a table', coroutine.create, setmetatable({}, {__call = function () end}))

	-- Yielding across a pcall works: pcall is yieldable in 5.4+.
	local across = coroutine.create(function ()
		local ok, value = pcall(function () return coroutine.yield('from pcall') end)
		return ok, value
	end)
	row('yield across pcall', coroutine.resume(across))
	row('resume back into pcall', coroutine.resume(across, 'resumed'))
	LUA);

?>
--EXPECT--
first resume = true, 5
second resume = true, 20
resume dead = false, "cannot resume dead coroutine"
status fresh = "suspended"
status of self = "running"
status suspended = "suspended", "suspended"
status dead = "dead"
running in main = "thread", true
running inside = "thread", false
isyieldable main = false
isyieldable inside = true
error via resume = false, "inside"
status after error = "dead"
error via wrap = "! wrapped"
wrap = 1, 2, 3, "done"
resume dead again = false, "cannot resume dead coroutine"
resume self = false, "cannot resume non-suspended coroutine"
close running = false, "cannot close a running coroutine"
close normal = false, "cannot close a normal coroutine"
close = true, "dead", {"closed"}
close again = true
yield in main = "! attempt to yield from outside a coroutine"
create a table = "! bad argument #1 to '?' (function expected, got table)"
yield across pcall = true, "from pcall"
resume back into pcall = true, true, "resumed"
