--TEST--
Lua conformance: the utf8 library, which is capability-gated but not modified
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

require __DIR__ . '/conformance.inc';

// utf8 is behind a capability in this build but the functions themselves are
// stock. Its value here is that it is the only part of the standard library that
// reasons about the CONTENT of a byte string, so a build that mangled string
// handling would show up here even if the byte-level tests did not.

conformance(<<<'LUA'
	local ascii = 'abc'
	local accented = 'na\xC3\xAFve'          -- naïve, 6 bytes, 5 codepoints
	local mixed = 'a\xE2\x82\xACb'           -- a€b, 5 bytes, 3 codepoints
	local astral = '\xF0\x9F\x98\x80'        -- U+1F600, 4 bytes, 1 codepoint

	-- char builds from codepoints; codepoint reads them back.
	row('char', utf8.char(72, 105), utf8.char(0x20AC) == '\xE2\x82\xAC')
	row('char astral', utf8.char(0x1F600) == astral)
	row('char empty', utf8.char())
	row('codepoint', utf8.codepoint(accented, 1), utf8.codepoint(accented, 3))
	row('codepoint range', utf8.codepoint(mixed, 1, -1))
	row('codepoint astral', utf8.codepoint(astral))

	-- len counts codepoints, # counts bytes, and the two differ exactly where
	-- multi-byte sequences appear.
	row('len vs #', utf8.len(ascii), #ascii, utf8.len(accented), #accented)
	row('len astral', utf8.len(astral), #astral)
	row('len range', utf8.len(mixed, 2), utf8.len(mixed, 1, 1))
	row('len of empty', utf8.len(''))

	-- An invalid sequence makes len return nil PLUS the offending position,
	-- which is what lets a host report where the data went wrong.
	row('len invalid', utf8.len('a\xFFb'))
	row('len truncated', utf8.len('a\xC3'))
	row('len continuation first', utf8.len('\x80'))

	-- offset converts a codepoint index to a byte index, and back with a
	-- negative n. Position past the end returns nil rather than erroring.
	--
	-- IT RETURNS TWO VALUES IN 5.5 -- where the character starts AND where it
	-- ends -- against one in 5.4. Code that spread it into a call's argument
	-- list silently gains an argument on this build, so the arity is pinned
	-- before the values are.
	row('offset arity', select('#', utf8.offset(accented, 1)))
	row('offset', utf8.offset(accented, 1), utf8.offset(accented, 3), utf8.offset(accented, 4))
	row('offset from end', utf8.offset(accented, -1))
	row('offset past end', utf8.offset(accented, 99))
	row('offset zero finds start', utf8.offset(accented, 0, 4))

	-- codes iterates position/codepoint pairs, and refuses invalid data rather
	-- than yielding a replacement character.
	local walked = {}
	for position, code in utf8.codes(mixed) do
		walked[#walked + 1] = position .. ':' .. code
	end
	row('codes', walked)
	try('codes on invalid', function ()
		for _ in utf8.codes('a\xFFb') do end
	end)

	-- charpattern matches one codepoint at a time, which is how a script does
	-- character-wise work with the pattern matcher.
	local chars = {}
	for character in accented:gmatch(utf8.charpattern) do chars[#chars + 1] = character end
	row('charpattern count', #chars)
	row('charpattern rebuilds', table.concat(chars) == accented)

	-- The lax flag accepts surrogates and out-of-range values that strict
	-- UTF-8 refuses. Both behaviours are Lua's, and a host relying on
	-- validation needs the strict one to actually be strict.
	row('strict rejects surrogate', utf8.len('\xED\xA0\x80'))
	row('lax accepts surrogate', utf8.len('\xED\xA0\x80', 1, -1, true))
	try('char out of range', utf8.char, 0x80000000)

	-- Byte functions are unaffected by codepoint boundaries, which is the trap
	-- this pair of libraries exists to make explicit.
	row('sub splits a codepoint', #accented:sub(1, 3), utf8.len(accented:sub(1, 3)))
	LUA, (new DevelopGravity\LuaExt\Capabilities())->with(utf8: true));

?>
--EXPECT--
char = "Hi", true
char astral = true
char empty = ""
codepoint = 110, 239
codepoint range = 97, 8364, 98
codepoint astral = 128512
len vs # = 3, 3, 5, 6
len astral = 1, 4
len range = 2, 1
len of empty = 0
len invalid = nil, 2
len truncated = nil, 2
len continuation first = nil, 1
offset arity = 2
offset = 1, 3, 5, 5
offset from end = 6, 6
offset past end = nil
offset zero finds start = 3, 4
codes = {"1:97", "2:8364", "5:98"}
codes on invalid = "! invalid UTF-8 code"
charpattern count = 5
charpattern rebuilds = true
strict rejects surrogate = nil, 1
lax accepts surrogate = 1
char out of range = "! bad argument #1 to '?' (value out of range)"
sub splits a codepoint = 3, nil, 3
