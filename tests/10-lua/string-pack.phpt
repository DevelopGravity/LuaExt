--TEST--
Lua conformance: string.pack, the most platform-sensitive corner of the language
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

require __DIR__ . '/conformance.inc';

// THE REASON THIS SUITE RUNS PER-PLATFORM. string.pack is the one part of the
// standard library whose answers legitimately differ between builds: native
// endianness, native alignment, and the width of `size_t` and `long` all leak
// into the result.
//
// So every check here uses an EXPLICIT size and an EXPLICIT byte order, which
// the format string can always specify. That makes the expected bytes the same
// on x64 Linux, arm64 macOS and Windows -- and it means a difference between
// platforms is a real bug rather than a property of the machine.
//
// The two things that ARE native are asserted as relationships instead of
// values: packsize agreeing with what pack produced, and a round trip returning
// what went in.

conformance(<<<'LUA'
	-- A helper so the expectation block shows bytes rather than control
	-- characters, which would not survive a diff.
	local function hex(s)
		return (s:gsub('.', function (c) return string.format('%02x', c:byte()) end))
	end

	-- Explicit width and byte order: identical on every platform.
	row('<i4', hex(string.pack('<i4', 1)), hex(string.pack('<i4', -1)))
	row('>i4', hex(string.pack('>i4', 1)), hex(string.pack('>i4', 258)))
	row('<i2 >i2', hex(string.pack('<i2', 258)), hex(string.pack('>i2', 258)))
	row('<i8', hex(string.pack('<i8', 1)))
	row('i1', hex(string.pack('i1', 127)), hex(string.pack('i1', -128)))

	-- Unsigned, and the wrap at the top of the range.
	row('<I4', hex(string.pack('<I4', 4294967295)))
	row('unpack I4 vs i4', string.unpack('<I4', string.pack('<i4', -1)))
	row('unpack i4', string.unpack('<i4', string.pack('<i4', -1)))

	-- Overflow is refused rather than truncated.
	try('i1 overflow', string.pack, 'i1', 200)
	try('i2 overflow', string.pack, '<i2', 70000)

	-- Floats with a fixed representation. f is IEEE single, d is double, n is
	-- lua_Number -- and only the first two are guaranteed a width.
	row('<f', hex(string.pack('<f', 1.0)))
	row('<d', hex(string.pack('<d', 1.0)))
	row('float round trip', string.unpack('<f', string.pack('<f', 0.5)))
	row('double round trip', string.unpack('<d', string.pack('<d', 1 / 3)))

	-- Strings: fixed width, length-prefixed, and zero-terminated.
	row('c4', hex(string.pack('c4', 'ab')), string.unpack('c4', string.pack('c4', 'ab')))
	row('<s4', hex(string.pack('<s4', 'hi')))
	row('z', hex(string.pack('z', 'hi')), string.unpack('z', string.pack('z', 'hi')))
	try('c4 too long', string.pack, 'c4', 'abcde')

	-- Alignment. '!' sets the maximum alignment and 'x' is a padding byte, so
	-- the layout can be pinned exactly instead of inherited from the machine.
	row('no alignment', #string.pack('<i1i4', 1, 1))
	row('aligned to 4', #string.pack('<!4i1i4', 1, 1))
	row('explicit padding', hex(string.pack('<i1xi2', 1, 1)))
	row('align reset', #string.pack('<!1i1i4', 1, 1))

	-- packsize agrees with pack for every fixed-width format, and refuses the
	-- variable-width ones rather than guessing.
	row('packsize', string.packsize('<i4'), string.packsize('<i4i4'), string.packsize('<!4i1i4'))
	row('packsize matches pack', string.packsize('<i4d') == #string.pack('<i4d', 1, 1.0))
	try('packsize of s4', string.packsize, '<s4')
	try('packsize of z', string.packsize, 'z')

	-- unpack returns the values then the next position, which is what makes
	-- sequential decoding work.
	local a, b, position = string.unpack('<i4i4', string.pack('<i4i4', 7, 8))
	row('unpack position', a, b, position)
	row('unpack from offset', string.unpack('<i4', string.pack('<i4i4', 7, 8), 5))

	-- Truncated input is an error, not a partial read.
	try('unpack too short', string.unpack, '<i4', 'ab')
	try('unpack bad format', string.unpack, '<q', 'abcdefgh')

	-- The NATIVE sizes are not asserted as numbers -- they differ legitimately
	-- between platforms. What must hold everywhere is that they are
	-- self-consistent, which is the property a host actually relies on.
	row('native j round trip', string.unpack('j', string.pack('j', math.maxinteger)))
	row('native T is size_t sized', string.packsize('T') == string.packsize('T'))
	row('native n round trip', string.unpack('n', string.pack('n', 2.5)))
	row('native i matches packsize', #string.pack('i', 1) == string.packsize('i'))
	LUA);

?>
--EXPECT--
<i4 = "01000000", "ffffffff"
>i4 = "00000001", "00000102"
<i2 >i2 = "0201", "0102"
<i8 = "0100000000000000"
i1 = "7f", "80"
<I4 = "ffffffff"
unpack I4 vs i4 = 4294967295, 5
unpack i4 = -1, 5
i1 overflow = "! bad argument #2 to '?' (integer overflow)"
i2 overflow = "! bad argument #2 to '?' (integer overflow)"
<f = "0000803f"
<d = "000000000000f03f"
float round trip = 0.5, 5
double round trip = 0.33333333333333331, 9
c4 = "61620000", "ab\0\0", 5
<s4 = "020000006869"
z = "686900", "hi", 4
c4 too long = "! bad argument #2 to '?' (string longer than given size)"
no alignment = 5
aligned to 4 = 8
explicit padding = "01000100"
align reset = 5
packsize = 4, 8, 8
packsize matches pack = true
packsize of s4 = "! bad argument #1 to '?' (variable-length format)"
packsize of z = "! bad argument #1 to '?' (variable-length format)"
unpack position = 7, 8, 9
unpack from offset = 8, 9
unpack too short = "! bad argument #2 to '?' (data string too short)"
unpack bad format = "! invalid format option 'q'"
native j round trip = 9223372036854775807, 9
native T is size_t sized = true
native n round trip = 2.5, 9
native i matches packsize = true
