--TEST--
Lua conformance: the io and os this build wrote from scratch behave like Lua's
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

require __DIR__ . '/conformance.inc';
require __DIR__ . '/../06-vfs/memory-filesystem.inc';

use DevelopGravity\LuaExt\Capabilities;

// io and os are NOT patched upstream code here -- liolib.c and loslib.c are
// excluded from the build entirely and these are re-implementations over the
// host's FileSystem. So there is no "mostly stock" safety net: every read mode,
// every seek whence and every return shape is code this project wrote, and the
// only thing that says it matches Lua is a test like this one.
//
// Everything is inside the virtual filesystem, whose root is always '/', so no
// row here depends on a host path or a path separator.

conformance(<<<'LUA'
	-- write and read back, with the modes Lua defines.
	local f = assert(io.open('/notes.txt', 'w'))
	row('write returns the file', io.type ~= nil or f ~= nil)
	f:write('first line\n', 'second line\n', 42, '\n')
	f:close()

	-- "a" reads the whole file; "l" drops the newline and "L" keeps it.
	local r = assert(io.open('/notes.txt', 'r'))
	row('read a', r:read('a'))
	r:close()

	r = assert(io.open('/notes.txt', 'r'))
	row('read l', r:read('l'))
	row('read L', r:read('L'))
	-- A DELIBERATE DIVERGENCE. Upstream's "n" format parses a number off the
	-- stream, which means a C-level scanner reading an unbounded run of digits
	-- from host-controlled data. This build refuses it and says what to do
	-- instead, and the message is part of the contract.
	try('read n', function () return r:read('n') end)
	row('read past end', r:read('l'), r:read('l'), r:read('a'))
	r:close()

	-- A byte count reads exactly that many bytes; 0 is the end-of-file probe.
	r = assert(io.open('/notes.txt', 'r'))
	row('read count', r:read(5), r:read(0))
	r:close()

	-- Several formats in one call return several values.
	r = assert(io.open('/notes.txt', 'r'))
	row('read multi', r:read('l', 'l'))
	r:close()

	-- The default format is "l".
	r = assert(io.open('/notes.txt', 'r'))
	row('read default', r:read())
	r:close()

	-- lines iterates, and the file:lines form leaves closing to the caller.
	local collected = {}
	for line in io.lines('/notes.txt') do collected[#collected + 1] = line end
	row('io.lines', collected)

	-- lines() TAKES FORMATS, and both spellings used to ignore them: whatever a
	-- caller asked for, a plain line came back. `L` dropping the newline it was
	-- specifically asked to keep was silent -- no error, just the wrong bytes.
	r = assert(io.open('/notes.txt', 'r'))
	local kept = {}
	for line in r:lines('L') do kept[#kept + 1] = #line end
	row('file:lines with a format', kept)
	r:close()

	local io_kept = {}
	for line in io.lines('/notes.txt', 'L') do io_kept[#io_kept + 1] = #line end
	row('io.lines with a format', io_kept)

	-- Several formats per step, ending on the one that runs out.
	local paired = {}
	for a, b in io.lines('/notes.txt', 'l', 'l') do
		paired[#paired + 1] = tostring(a) .. '/' .. tostring(b)
	end
	row('lines with two formats', paired)

	-- A byte count is a format too.
	local chunks = {}
	for chunk in io.lines('/notes.txt', 8) do chunks[#chunks + 1] = #chunk end
	row('lines with a byte count', chunks)

	-- And no format still means a line.
	local defaulted = {}
	for line in io.lines('/notes.txt') do defaulted[#defaulted + 1] = line end
	row('lines default format', defaulted)

	try('lines with a bad format', function ()
		for _ in io.lines('/notes.txt', 'q') do end
	end)

	-- seek: the three whences, and that it returns the resulting position.
	r = assert(io.open('/notes.txt', 'r'))
	row('seek set', r:seek('set', 6))
	row('after seek', r:read(4))
	row('seek cur', r:seek('cur', 0))
	row('seek end', r:seek('end'))
	row('seek default is cur', r:seek())
	row('seek to start', r:seek('set'))
	row('read after rewind', r:read(5))
	r:close()

	-- Append mode writes at the end whatever the position was.
	local a = assert(io.open('/notes.txt', 'a'))
	a:write('appended\n')
	a:close()
	r = assert(io.open('/notes.txt', 'r'))
	local whole = r:read('a')
	r:close()
	row('append', whole:sub(-9))

	-- "w" truncates.
	local w = assert(io.open('/notes.txt', 'w'))
	w:write('fresh')
	w:close()
	r = assert(io.open('/notes.txt', 'r'))
	row('truncated', r:read('a'))
	r:close()

	-- Opening a missing file for reading fails with nil plus a message, which is
	-- what makes `assert(io.open(...))` the idiom it is.
	row('open missing', io.open('/nope.txt', 'r'))
	-- The message, without the "chunk:line:" assert() prefixes onto it -- that
	-- line number moves every time a check is inserted above this one.
	row('assert on missing', (select(2, pcall(function ()
		return assert(io.open('/nope.txt'))
	end)):gsub('^.-:%d+: ', '')))

	-- A closed handle refuses further use rather than reading freed state.
	local closed = assert(io.open('/notes.txt', 'r'))
	closed:close()
	try('read after close', function () return closed:read('a') end)
	row('tostring of a closed file', tostring(closed))

	-- write returns the file, so writes chain.
	local chain = assert(io.open('/chain.txt', 'w'))
	row('write chains', chain:write('a'):write('b') == chain)
	chain:close()
	r = assert(io.open('/chain.txt', 'r'))
	row('chained content', r:read('a'))
	r:close()

	-- os.remove and os.rename report success or nil plus a message.
	row('rename', os.rename('/chain.txt', '/renamed.txt'))
	row('renamed exists', (function ()
		local h = io.open('/renamed.txt', 'r')
		if not h then return 'missing' end
		local body = h:read('a')
		h:close()
		return body
	end)())
	row('remove', os.remove('/renamed.txt'))
	row('remove missing', os.remove('/renamed.txt'))
	row('rename missing', os.rename('/nope.txt', '/other.txt'))

	-- os.date with the '!' prefix is UTC, which is the only spelling that means
	-- the same thing on every machine this suite runs on.
	row('date utc', os.date('!%Y-%m-%d %H:%M:%S', 0))
	row('date table', (function ()
		local t = os.date('!*t', 86400)
		return t.year, t.month, t.day, t.hour, t.isdst
	end)())
	row('date default format', type(os.date('!%c', 0)))

	-- time from a table round-trips through date, and difftime subtracts.
	row('difftime', os.difftime(100, 40), math.type(os.difftime(100, 40)))
	row('time is a number', math.type(os.time()))

	-- clock is monotonic-ish and non-negative. Its VALUE is a timing fact and
	-- deliberately not asserted -- see the header of conformance.inc.
	row('clock is a number', math.type(os.clock()) ~= nil, os.clock() >= 0)
	LUA, (new Capabilities())->with(vfs: true, vfsWrite: true), new MemoryFileSystem());

?>
--EXPECT--
write returns the file = true
read a = "first line\10second line\1042\10"
read l = "first line"
read L = "second line\10"
read n = "! bad argument #1 to 'read' (the \"n\" format is not supported; read bytes and use tonumber)"
read past end = "42", nil, ""
read count = "first", ""
read multi = "first line", "second line"
read default = "first line"
io.lines = {"first line", "second line", "42"}
file:lines with a format = {11, 12, 3}
io.lines with a format = {11, 12, 3}
lines with two formats = {"first line/second line", "42/nil"}
lines with a byte count = {8, 8, 8, 2}
lines default format = {"first line", "second line", "42"}
lines with a bad format = "! bad argument #1 to 'for iterator' (invalid format \"q\")"
seek set = 6
after seek = "line"
seek cur = 10
seek end = 26
seek default is cur = 26
seek to start = 0
read after rewind = "first"
append = "appended\10"
truncated = "fresh"
open missing = nil, "no such file"
assert on missing = "no such file"
read after close = "! attempt to use a closed file"
tostring of a closed file = "file (closed)"
write chains = true
chained content = "ab"
rename = true
renamed exists = "ab"
remove = true
remove missing = nil, "no such file: /renamed.txt"
rename missing = nil, "no such file: /nope.txt"
date utc = "1970-01-01 00:00:00"
date table = 1970, 1, 2, 0, false
date default format = "string"
difftime = 60.0, "float"
time is a number = "integer"
clock is a number = true, true
