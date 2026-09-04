--TEST--
Lua conformance: the math library, and the randomseed this build replaced
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

require __DIR__ . '/conformance.inc';

// math.randomseed is replaced in this build: upstream's returns the address-
// derived components it seeded from, which hands a script a process address.
// The replacement is void and integer-only. Everything else in the library must
// be stock, and the integer/float boundary cases are where a patched build would
// most plausibly drift.
//
// Nothing here asserts a transcendental's exact bits: sin/exp/log are the C
// library's and may differ in the last place between platforms. Where one is
// checked, it is checked to a tolerance or through an identity.

conformance(<<<'LUA'
	-- floor and ceil return INTEGERS when they can, which is not obvious and is
	-- what makes them usable as table keys.
	row('floor', math.floor(3.7), math.type(math.floor(3.7)), math.floor(-3.2))
	row('ceil', math.ceil(3.2), math.ceil(-3.7))
	row('floor of integer', math.floor(3), math.type(math.floor(3)))
	row('floor of huge float', math.type(math.floor(1e300)))

	-- abs on mininteger wraps, because negating it is not representable.
	row('abs', math.abs(-5), math.abs(5), math.abs(-5.5))
	row('abs mininteger wraps', math.abs(math.mininteger) == math.mininteger)

	-- max and min preserve the subtype of whichever argument wins.
	row('max min', math.max(1, 2, 3), math.min(1, 2, 3))
	row('max keeps subtype', math.type(math.max(1, 2.5)), math.type(math.max(2.5, 3)))
	try('max of nothing', math.max)

	-- fmod keeps the sign of the DIVIDEND; the % operator keeps the divisor's.
	-- The pair is the single most confusable thing in this library.
	row('fmod vs %', math.fmod(-7, 3), -7 % 3)
	row('fmod positive', math.fmod(7, 3), math.fmod(7, -3))
	row('fmod integers stay integer', math.type(math.fmod(7, 3)))
	try('fmod by zero', math.fmod, 7, 0)

	-- tointeger converts only when the value is exactly an integer.
	row('tointeger', math.tointeger(3.0), math.tointeger(3), math.tointeger(3.5))
	row('tointeger of a string', math.tointeger('3'))
	row('tointeger out of range', math.tointeger(1e300))

	-- type distinguishes the two number subtypes and returns nil otherwise.
	row('math.type', math.type(1), math.type(1.0), math.type('1'), math.type(nil))

	-- ult compares as UNSIGNED, which is the only way to treat -1 as large.
	row('ult', math.ult(1, 2), math.ult(-1, 1), math.ult(1, -1))

	-- The limits, and that they are integers.
	row('limits', math.maxinteger, math.mininteger)
	row('limit types', math.type(math.maxinteger), math.type(math.mininteger))
	row('huge', math.huge > 0, -math.huge < 0, math.type(math.huge))
	row('pi', ('%0.5f'):format(math.pi))

	-- Float-to-string is ROUND-TRIP exact in 5.5, not the 14 significant
	-- digits 5.4 used: LUA_NUMBER_FMT_N is %.17g and tostring goes through
	-- it. A host that parses these back gets the same double it started
	-- with, which %.14g did not guarantee.
	row('tostring is round-trip', tostring(1 / 3), tostring(0.1))
	row('format is not', ('%.14g'):format(1 / 3), ('%g'):format(1 / 3))
	row('round trip really round-trips', tonumber(tostring(1 / 3)) == 1 / 3)

	-- sqrt and the transcendentals, to a tolerance rather than to the bit.
	row('sqrt', math.sqrt(16), math.type(math.sqrt(16)))
	row('sqrt of negative is nan', math.sqrt(-1) ~= math.sqrt(-1))
	row('exp/log identity', math.abs(math.log(math.exp(1)) - 1) < 1e-12)
	row('log base', math.abs(math.log(8, 2) - 3) < 1e-12)
	row('sin/cos identity', math.abs(math.sin(1) ^ 2 + math.cos(1) ^ 2 - 1) < 1e-12)

	-- THE REPLACED FUNCTION. Void, integer-only, and still actually seeding.
	row('randomseed returns nothing', select('#', math.randomseed(1)))
	try('randomseed with no argument', math.randomseed)
	try('randomseed with a float', math.randomseed, 1.5)

	math.randomseed(99)
	local first = {math.random(1, 1000000), math.random(1, 1000000)}
	math.randomseed(99)
	local second = {math.random(1, 1000000), math.random(1, 1000000)}
	row('seeding is deterministic', first[1] == second[1] and first[2] == second[2])

	-- random's three arities, checked by range rather than by value.
	local zero_one = math.random()
	row('random()', zero_one >= 0 and zero_one < 1, math.type(zero_one))
	local upto = math.random(10)
	row('random(m)', upto >= 1 and upto <= 10, math.type(upto))
	local between = math.random(5, 6)
	row('random(m, n)', between == 5 or between == 6)
	row('random(0) is an integer', math.type(math.random(0)))
	try('random empty range', math.random, 5, 1)
	LUA);

?>
--EXPECT--
floor = 3, "integer", -4
ceil = 4, -3
floor of integer = 3, "integer"
floor of huge float = "float"
abs = 5, 5, 5.5
abs mininteger wraps = true
max min = 3, 1
max keeps subtype = "float", "integer"
max of nothing = "! bad argument #1 to '?' (value expected)"
fmod vs % = -1, 2
fmod positive = 1, 1
fmod integers stay integer = "integer"
fmod by zero = "! bad argument #2 to '?' (zero)"
tointeger = 3, 3, nil
tointeger of a string = 3
tointeger out of range = nil
math.type = "integer", "float", nil, nil
ult = true, false, true
limits = 9223372036854775807, -9223372036854775808
limit types = "integer", "integer"
huge = true, true, "float"
pi = "3.14159"
tostring is round-trip = "0.33333333333333331", "0.1"
format is not = "0.33333333333333", "0.333333"
round trip really round-trips = true
sqrt = 4.0, "float"
sqrt of negative is nan = true
exp/log identity = true
log base = true
sin/cos identity = true
randomseed returns nothing = 0
randomseed with no argument = "! bad argument #1 to '?' (number expected, got no value)"
randomseed with a float = "! bad argument #1 to '?' (number has no integer representation)"
seeding is deterministic = true
random() = true, "float"
random(m) = true, "integer"
random(m, n) = true
random(0) is an integer = "integer"
random empty range = "! bad argument #1 to '?' (interval is empty)"
