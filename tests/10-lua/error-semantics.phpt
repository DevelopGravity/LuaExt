--TEST--
Lua conformance: error, assert, pcall and xpcall keep their ordinary shapes
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

require __DIR__ . '/conformance.inc';

// pcall, xpcall and error are all REPLACED in this build so that a fatal cannot
// be caught -- tests/03-adversarial/ proves the fatal half. This file proves the
// other half, which is easier to break and harder to notice: that an ORDINARY
// error still behaves exactly as Lua says. A replacement that quietly stringified
// a table error, or dropped a return value, or added a position prefix where
// upstream adds none, would pass every adversarial test in the suite.

conformance(<<<'LUA'
	-- pcall returns true plus every return value, or false plus the error.
	row('pcall ok', pcall(function () return 1, 2, 3 end))
	row('pcall count', select('#', pcall(function () return 1, 2, 3 end)))
	row('pcall with args', pcall(function (a, b) return a + b end, 2, 3))
	row('pcall of a non-function', pcall(42))
	row('pcall of nil', pcall(nil))

	-- error() with a string gets a "chunk:line:" prefix at the default level,
	-- and none at level 0.
	local _, prefixed = pcall(function () error('with position') end)
	local _, bare = pcall(function () error('no position', 0) end)
	row('error level 1 has a prefix', prefixed:find('conformance:%d+: with position') ~= nil)
	row('error level 0', bare)

	-- level 2 blames the CALLER, which is how an argument-checking helper points
	-- at the code that passed the bad argument.
	local function blames_caller() error('your fault', 2) end
	local _, blamed = pcall(function () blames_caller() end)
	row('error level 2 still has a prefix', blamed:find('conformance:%d+: your fault') ~= nil)

	-- A NON-STRING error value passes through untouched: no prefix, no
	-- stringification. Hosts rely on this to throw structured errors.
	local _, table_error = pcall(function () error({code = 42}) end)
	row('table error', type(table_error), table_error.code)

	local _, number_error = pcall(function () error(404) end)
	row('number error', number_error, math.type(number_error))

	local _, nil_error = pcall(function () error() end)
	row('error with no argument', nil_error)

	local _, false_error = pcall(function () error(false) end)
	row('error false', false_error)

	-- assert returns ALL its arguments on success, which is what makes
	-- `local f = assert(io.open(...))` idiomatic.
	row('assert passthrough', assert(1, 2, 3))
	row('assert message', select(2, pcall(assert, false, 'custom')))
	row('assert default message', select(2, pcall(assert, false)))
	row('assert nil', select(2, pcall(assert, nil)))
	row('assert non-string message', type(select(2, pcall(assert, false, {}))))
	-- 0 is TRUTHY in Lua, unlike almost every other language a host author
	-- knows, so assert(0) passes and returns the 0.
	row('assert 0 is truthy', select(2, pcall(assert, 0)))

	-- xpcall runs a handler on the error, and the handler's return value becomes
	-- the second result.
	row('xpcall ok', xpcall(function () return 'fine' end, function () return 'handled' end))
	row('xpcall handled', xpcall(function () error('bad', 0) end, function (e) return 'saw: ' .. e end))
	row('xpcall handler sees table', xpcall(
		function () error({code = 7}) end,
		function (e) return type(e) == 'table' and e.code end))

	-- xpcall passes extra arguments to the protected function, not the handler.
	row('xpcall args', xpcall(function (a, b) return a .. b end, function () return 'h' end, 'x', 'y'))

	-- An error raised INSIDE the handler does not loop forever.
	row('error in handler', xpcall(function () error('first', 0) end, function () error('second', 0) end))

	-- pcall nests, and the inner one catches first.
	row('nested pcall', pcall(function ()
		local ok, err = pcall(function () error('inner', 0) end)
		return 'outer saw ' .. tostring(ok) .. '/' .. tostring(err)
	end))

	-- select('#') on a pcall result is how a caller distinguishes "returned
	-- nothing" from "returned nil".
	row('returned nothing', select('#', pcall(function () end)))
	row('returned nil', select('#', pcall(function () return nil end)))

	-- Runtime errors carry the standard messages, which hosts match on.
	try('call a nil', function () local f = nil return f() end)
	try('index a nil', function () local t = nil return t.x end)
	try('arith on nil', function () return nil + 1 end)
	try('compare mismatched', function () return {} < {} end)
	try('length of a number', function () return #5 end)

	-- A named local appears in the message, which is most of its diagnostic
	-- value and is easy to lose when a VM is patched.
	try('names the local', function () local missing return missing.field end)
	try('names the global', function () return undefined_global.field end)
	LUA);

?>
--EXPECT--
pcall ok = true, 1, 2, 3
pcall count = 4
pcall with args = true, 5
pcall of a non-function = false, "attempt to call a number value"
pcall of nil = false, "attempt to call a nil value"
error level 1 has a prefix = true
error level 0 = "no position"
error level 2 still has a prefix = true
table error = "table", 42
number error = 404, "integer"
error with no argument = "<no error object>"
error false = false
assert passthrough = 1, 2, 3
assert message = "custom"
assert default message = "assertion failed!"
assert nil = "assertion failed!"
assert non-string message = "table"
assert 0 is truthy = 0
xpcall ok = true, "fine"
xpcall handled = false, "saw: bad"
xpcall handler sees table = false, 7
xpcall args = true, "xy"
error in handler = false, "error in error handling"
nested pcall = true, "outer saw false/inner"
returned nothing = 1
returned nil = 2
call a nil = "! attempt to call a nil value (local 'f')"
index a nil = "! attempt to index a nil value (local 't')"
arith on nil = "! attempt to perform arithmetic on a nil value"
compare mismatched = "! attempt to compare two table values"
length of a number = "! attempt to get length of a number value"
names the local = "! attempt to index a nil value (local 'missing')"
names the global = "! attempt to index a nil value (global 'undefined_global')"
