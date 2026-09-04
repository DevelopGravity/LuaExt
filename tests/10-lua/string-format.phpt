--TEST--
Lua conformance: string.format, including the %p this build refuses
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

require __DIR__ . '/conformance.inc';

// string.format is one of the few places this build deliberately DIFFERS from
// stock Lua: %p is rejected outright, because it prints an address and the
// format string itself is too useful to withhold. See SECURITY.md on ASLR
// disclosure. Everything else must behave exactly as upstream.
//
// Nothing here depends on a locale: the vendored Lua is compiled in strict ISO C
// mode with no setlocale, so the decimal point is always '.' on every platform
// this runs on.

conformance(<<<'LUA'
	-- Integer directives, including width, zero-fill and sign.
	row('%d', ('%d'):format(42), ('%5d'):format(42), ('%-5d|'):format(42), ('%05d'):format(42))
	row('%d sign', ('%+d'):format(42), ('%+d'):format(-42), ('% d'):format(42))
	row('%i %u', ('%i'):format(7), ('%u'):format(7))
	row('%x %X %o', ('%x'):format(255), ('%X'):format(255), ('%o'):format(8), ('%#x'):format(255))
	row('%c', ('%c'):format(65))

	-- A float in an integer slot is refused unless it is exactly integral.
	row('%d on 3.0', ('%d'):format(3.0))
	try('%d on 3.5', string.format, '%d', 3.5)
	try('%d on a string', string.format, '%d', 'x')

	-- Float directives. %.14g is what Lua's own tostring uses.
	row('%f', ('%f'):format(1.5), ('%.2f'):format(1.005), ('%.0f'):format(2.5))
	row('%e %g', ('%e'):format(1500), ('%g'):format(1500), ('%g'):format(0.0001))
	row('%.14g', ('%.14g'):format(1 / 3))
	row('%a is hex float', (('%a'):format(1.0)):sub(1, 2))

	-- %s takes anything tostring() takes, and honours precision as truncation.
	row('%s', ('%s'):format('abc'), ('%10s|'):format('ab'), ('%-10s|'):format('ab'))
	row('%s precision', ('%.2s'):format('abcdef'))
	row('%s coerces', ('%s %s %s'):format(1, 1.5, true))
	row('%s uses __tostring', ('%s'):format(setmetatable({}, {__tostring = function () return 'T' end})))

	-- %q is the round-trip quoter. Its job is that load() reads back what it
	-- wrote, so the escapes matter more than the exact spelling.
	row('%q', ('%q'):format('he said "hi"'))
	row('%q backslash', ('%q'):format('a\\b'))
	row('%q nul', ('%q'):format('a\0b'))
	row('%q integer', ('%q'):format(42))
	row('%q true', ('%q'):format(true), ('%q'):format(nil))

	-- %% is a literal percent and consumes no argument.
	row('%%', ('100%%'):format(), ('%d%%'):format(50))

	-- Missing and surplus arguments.
	try('missing argument', string.format, '%d %d', 1)
	row('surplus argument', ('%d'):format(1, 2, 3))

	-- THE DELIBERATE DIFFERENCE. %p prints an address in stock Lua and is
	-- refused here, in every position and combination.
	try('%p', string.format, '%p', {})
	try('%p on a string', string.format, '%p', 'x')
	try('%p mid-format', string.format, 'a %p b', {})
	try('%p with width', string.format, '%20p', {})

	-- An unknown directive is an error rather than being passed through.
	try('%z', string.format, '%z', 1)
	try('trailing %', string.format, '100%', 1)
	LUA);

?>
--EXPECT--
%d = "42", "   42", "42   |", "00042"
%d sign = "+42", "-42", " 42"
%i %u = "7", "7"
%x %X %o = "ff", "FF", "10", "0xff"
%c = "A"
%d on 3.0 = "3"
%d on 3.5 = "! bad argument #2 to '?' (number has no integer representation)"
%d on a string = "! bad argument #2 to '?' (number expected, got string)"
%f = "1.500000", "1.00", "2"
%e %g = "1.500000e+03", "1500", "0.0001"
%.14g = "0.33333333333333"
%a is hex float = "0x"
%s = "abc", "        ab|", "ab        |"
%s precision = "ab"
%s coerces = "1 1.5 true"
%s uses __tostring = "T"
%q = "\"he said \\\"hi\\\"\""
%q backslash = "\"a\\\\b\""
%q nul = "\"a\\0b\""
%q integer = "42"
%q true = "true", "nil"
%% = "100%", "50%"
missing argument = "! bad argument #3 to '?' (no value)"
surplus argument = "1"
%p = "! invalid conversion '%p' to 'format'"
%p on a string = "! invalid conversion '%p' to 'format'"
%p mid-format = "! invalid conversion '%p' to 'format'"
%p with width = "! invalid conversion '%p' to 'format'"
%z = "! invalid conversion '%z' to 'format'"
trailing % = "! invalid conversion '%' to 'format'"
