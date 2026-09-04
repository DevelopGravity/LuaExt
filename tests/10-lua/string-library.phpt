--TEST--
Lua conformance: the string library, including its negative-index rules
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

require __DIR__ . '/conformance.inc';

// Lua strings are byte strings, 1-indexed, with negative indices counting from
// the end and out-of-range indices clamped rather than refused. Every one of
// those rules is a place a host author gets it wrong, so each is pinned here.

conformance(<<<'LUA'
	-- sub: the clamping rules are the whole point.
	row('sub', ('hello'):sub(2, 4), ('hello'):sub(2), ('hello'):sub(-3))
	row('sub negative', ('hello'):sub(-3, -2), ('hello'):sub(2, -2))
	row('sub clamped', ('hello'):sub(0), ('hello'):sub(-99), ('hello'):sub(1, 99))
	row('sub empty', ('hello'):sub(4, 2), ('hello'):sub(9), (''):sub(1, 5))

	-- byte and char round-trip, including multiple returns and the 0 byte.
	row('byte', ('ABC'):byte(), ('ABC'):byte(2), ('ABC'):byte(1, -1))
	row('byte out of range', select('#', ('ABC'):byte(9)))
	row('char', string.char(72, 105), string.char())
	row('NUL is a byte', #string.char(0, 65, 0), (string.char(0, 65)):byte(2))
	try('char out of range', string.char, 256)
	try('char negative', string.char, -1)

	-- len counts bytes, not characters, and # agrees with it.
	local utf8_text = 'na\xC3\xAFve'
	row('len is bytes', utf8_text:len(), #utf8_text)

	-- rep, with and without a separator, and the degenerate counts.
	row('rep', ('ab'):rep(3), ('ab'):rep(3, '-'))
	row('rep zero', ('ab'):rep(0), ('ab'):rep(-1))
	row('rep one has no sep', ('ab'):rep(1, '-'))

	-- reverse is byte-wise, which is why it mangles multi-byte text. That is
	-- correct and worth showing rather than hiding.
	row('reverse', ('abc'):reverse(), (''):reverse())
	row('reverse is bytes', #(utf8_text:reverse()))

	-- upper and lower are ASCII-only here: the vendored build is compiled in
	-- strict ISO C mode with no setlocale, so nothing can change what they fold.
	row('case', ('Hello'):upper(), ('Hello'):lower())
	row('case leaves non-ascii', (utf8_text:upper()) == 'NA\xC3\xAFVE')

	-- The string metatable is why 'x':method() works, and it is also reachable
	-- as string.method(x).
	row('two spellings', string.upper('a'), ('a'):upper())

	-- Numbers coerce to strings for string functions, one of Lua's older
	-- conveniences. The result is the same text tostring() gives.
	row('number coercion', string.upper(12), string.len(1.5), ('%d'):format(3))

	-- Comparison is byte order, not locale collation.
	row('ordering', 'a' < 'b', 'A' < 'a', 'abc' < 'abd', '' < 'a')

	-- Concatenation of many pieces, the path table.concat exists to avoid.
	local built = ''
	for index = 1, 5 do built = built .. index end
	row('concat loop', built)
	LUA);

?>
--EXPECT--
sub = "ell", "ello", "llo"
sub negative = "ll", "ell"
sub clamped = "hello", "hello", "hello"
sub empty = "", "", ""
byte = 65, 66, 65, 66, 67
byte out of range = 0
char = "Hi", ""
NUL is a byte = 3, 65
char out of range = "! bad argument #1 to '?' (value out of range)"
char negative = "! bad argument #1 to '?' (value out of range)"
len is bytes = 6, 6
rep = "ababab", "ab-ab-ab"
rep zero = "", ""
rep one has no sep = "ab"
reverse = "cba", ""
reverse is bytes = 6
case = "HELLO", "hello"
case leaves non-ascii = true
two spellings = "A", "A"
number coercion = "12", 3, "3"
ordering = true, true, true, true
concat loop = "12345"
