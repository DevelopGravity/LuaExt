--TEST--
Lua conformance: <close> runs in reverse order, on error as well as on success
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

require __DIR__ . '/conformance.inc';

// A to-be-closed variable is the only Lua construct that runs code during an
// UNWIND, which makes it the natural place for a script to try to outlive its
// own limit breach -- tests/03-adversarial/close-cannot-outlive-limit.phpt
// covers that half. This file covers the other half: that the ordinary,
// non-adversarial semantics are still exactly Lua's.

conformance(<<<'LUA'
	local function closer(log, name, err)
		return setmetatable({}, {__close = function ()
			log[#log + 1] = name
			if err then error(err, 0) end
		end})
	end

	-- Reverse order of declaration, at the end of the enclosing block.
	local order = {}
	do
		local a <close> = closer(order, 'a')
		local b <close> = closer(order, 'b')
		local c <close> = closer(order, 'c')
		order[#order + 1] = 'body'
	end
	row('reverse order', order)

	-- The block boundary is what closes it, not the end of the function.
	local scoped = {}
	local function has_inner_block()
		do
			local x <close> = closer(scoped, 'inner')
		end
		scoped[#scoped + 1] = 'after block'
	end
	has_inner_block()
	row('closes at block end', scoped)

	-- A return closes on the way out, and the return value is computed first.
	local returned = {}
	local function returns_early()
		local x <close> = closer(returned, 'closed')
		return 'value'
	end
	row('return closes', returns_early(), returned)

	-- break and goto close too.
	local looped = {}
	for _ = 1, 3 do
		local x <close> = closer(looped, 'iteration')
		break
	end
	row('break closes', looped)

	-- AN ERROR STILL CLOSES, which is the whole reason the feature exists.
	local unwound = {}
	local ok, err = pcall(function ()
		local a <close> = closer(unwound, 'outer')
		local b <close> = closer(unwound, 'inner')
		error('boom', 0)
	end)
	row('error closes', ok, err, unwound)

	-- A false or nil value needs no __close; anything else without one is
	-- refused at the point of assignment.
	row('nil is closable', (function () local x <close> = nil return 'ok' end)())
	row('false is closable', (function () local x <close> = false return 'ok' end)())
	try('non-closable value', function () local x <close> = {} end)
	try('non-closable number', function () local x <close> = 5 end)

	-- An error raised INSIDE __close propagates, and does not stop the others
	-- from running.
	local mixed = {}
	local ok2, err2 = pcall(function ()
		local a <close> = closer(mixed, 'a')
		local b <close> = closer(mixed, 'b-raises', 'from __close')
		local c <close> = closer(mixed, 'c')
	end)
	row('error in __close', ok2, err2, mixed)

	-- __close receives the error that caused the unwind, or nil on a clean exit.
	local seen = {}
	pcall(function ()
		local x <close> = setmetatable({}, {__close = function (_, e) seen[#seen + 1] = tostring(e) end})
		error('cause', 0)
	end)
	do
		local y <close> = setmetatable({}, {__close = function (_, e) seen[#seen + 1] = tostring(e) end})
	end
	row('__close sees the error', seen)

	-- <const> is compile-time: the value is folded and cannot be assigned. The
	-- assignment case is a compile error and lives in language-semantics.phpt;
	-- what is observable at runtime is that the binding works normally.
	local limit <const> = 10
	row('const reads', limit, limit * 2)

	-- A <const> whose value is a compile-time constant may be used where a
	-- constant is required, which is the point of the attribute.
	local size <const> = 4
	row('const in an expression', ('x'):rep(size))
	LUA);

?>
--EXPECT--
reverse order = {"body", "c", "b", "a"}
closes at block end = {"inner", "after block"}
return closes = "value", {"closed"}
break closes = {"iteration"}
error closes = false, "boom", {"inner", "outer"}
nil is closable = "ok"
false is closable = "ok"
non-closable value = "! variable 'x' got a non-closable value"
non-closable number = "! variable 'x' got a non-closable value"
error in __close = false, "from __close", {"c", "b-raises", "a"}
__close sees the error = {"cause", "nil"}
const reads = 10, 20
const in an expression = "xxxx"
