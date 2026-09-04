--TEST--
Lua conformance: core language semantics still compute the right answers
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

require __DIR__ . '/conformance.inc';

use DevelopGravity\LuaExt\Sandbox;

// The interpreter is vendored and patched, and the patches sit in lvm.c's
// dispatch loop. Arithmetic, the integer/float split and control flow all run
// through it, so this file is the closest thing to a smoke test for "did the
// interrupt checks change what the VM computes".

conformance(<<<'LUA'
	-- Integer and float are distinct subtypes with one equality.
	row('int subtype', math.type(3), math.type(3.0), math.type('3'))
	row('3 == 3.0', 3 == 3.0)
	row('3 // 1 type', math.type(3 // 1), math.type(3.0 // 1))

	-- Division always floats; floor division does not.
	row('7 / 2', 7 / 2)
	row('7 // 2', 7 // 2)
	row('7.0 // 2', 7.0 // 2)

	-- Floor division and modulo on negatives: the quotient floors toward minus
	-- infinity and the remainder takes the sign of the DIVISOR, which is where
	-- Lua differs from C and from most languages a host author knows.
	row('-7 // 2', -7 // 2)
	row('7 // -2', 7 // -2)
	row('-7 // -2', -7 // -2)
	row('-7 % 3', -7 % 3)
	row('7 % -3', 7 % -3)
	row('-7 % -3', -7 % -3)
	row('5.5 % 2', 5.5 % 2)
	row('-5.5 % 2', -5.5 % 2)

	-- Integers wrap; they do not promote and they do not saturate.
	row('maxint + 1', math.maxinteger + 1 == math.mininteger)
	row('minint - 1', math.mininteger - 1 == math.maxinteger)
	row('maxint', math.maxinteger)
	row('minint', math.mininteger)

	-- Integer division and modulo by zero are errors; float division is not.
	try('1 // 0', function () return 1 // 0 end)
	try('1 % 0', function () return 1 % 0 end)
	row('1.0 // 0', 1.0 // 0)
	row('1 / 0', 1 / 0)
	row('-1 / 0', -1 / 0)
	row('0 / 0 ~= itself', 0 / 0 ~= 0 / 0)

	-- Bitwise operators are integer-only, and a float with no integer
	-- representation is refused rather than truncated.
	row('bits', 0xF0 | 0x0F, 0xFF & 0x0F, 0xFF ~ 0x0F, ~0, 1 << 4, 256 >> 4)
	row('shift over width', 1 << 64, -1 >> 63)
	try('3.5 | 1', function () return 3.5 | 1 end)
	row('3.0 | 1', 3.0 | 1)

	-- Exponentiation is always float, even on integer operands.
	row('2^10', 2 ^ 10, math.type(2 ^ 10))

	-- String/number coercion in arithmetic, and its absence in comparison.
	row('"10" + 5', '10' + 5, math.type('10' + 5))
	row('"0x10" + 0', '0x10' + 0)
	row('"1e2" + 0', '1e2' + 0, math.type('1e2' + 0))
	try('"ten" + 1', function () return 'ten' + 1 end)
	try('"1" < 2', function () return '1' < 2 end)
	row('10 .. 20', 10 .. 20)
	row('1.0 .. ""', 1.0 .. '')

	-- tonumber's bases and its refusals.
	row('tonumber', tonumber('  12  '), tonumber('ff', 16), tonumber('z', 36), tonumber('10', 2))
	row('tonumber fail', tonumber('12a'), tonumber(''), tonumber('0x'))
	-- A base outside 2..36 is an argument error, not a nil result: the string is
	-- unreadable in a base that does not exist, which is the caller's mistake.
	try('tonumber base 37', tonumber, '1', 37)

	-- Length, and the truthiness rules: only nil and false are false.
	row('#', #'hello', #{1, 2, 3})
	row('truthy', not not 0, not not '', not not nil, not not false)

	-- Multiple returns truncate everywhere but the last position.
	local function three() return 1, 2, 3 end
	row('three()', three())
	row('(three())', (three()))
	row('in middle', three(), 'after')
	local a, b, c, d = three()
	row('assignment', a, b, c, d)
	row('table.pack', table.pack(three()).n)
	row('in table', #{three()}, #{three(), 'x'})

	-- select, including the negative index form.
	row('select #', select('#'), select('#', nil), select('#', 1, nil, 3))
	row('select 2', select(2, 'a', 'b', 'c'))
	row('select -1', select(-1, 'a', 'b', 'c'))
	try('select 0', function () return select(0, 'a') end)

	-- Varargs, and that they are not a table.
	local function collect(...)
		local packed = table.pack(...)
		return packed.n, ...
	end
	row('varargs', collect('x', nil, 'z'))

	-- Closures over the SAME variable share it; a fresh loop iteration does not.
	local function counter()
		local n = 0
		return function () n = n + 1 return n end, function () return n end
	end
	local bump, peek = counter()
	bump()
	bump()
	row('shared upvalue', peek())

	local made = {}
	for index = 1, 3 do made[index] = function () return index end end
	row('per-iteration', made[1](), made[2](), made[3]())

	-- goto, as the continue Lua does not have.
	local kept = {}
	for index = 1, 6 do
		if index % 2 == 0 then goto continue end
		kept[#kept + 1] = index
		::continue::
	end
	row('goto continue', kept)

	-- Numeric for: a float step makes the variable float, and a zero step is an
	-- error rather than a hang. (The control variable itself is const in 5.5 --
	-- asserted from PHP below, because that is a compile error and nothing
	-- inside a chunk can catch its own failure to compile.)
	local floats = {}
	for value = 1, 2, 0.5 do floats[#floats + 1] = value end
	row('float step', floats)

	local backwards = {}
	for value = 3, 1, -1 do backwards[#backwards + 1] = value end
	row('negative step', backwards)

	try('zero step', function () for _ = 1, 2, 0 do end end)

	local never = 0
	for _ = 3, 1 do never = never + 1 end
	row('empty range', never)
	LUA);

// Compile-time rules, which no chunk can report about itself. validate() answers
// as data, so they read like any other row.
//
// The const control variable is a 5.4 -> 5.5 change and worth pinning: code
// written against 5.4 that reassigns the loop variable stops compiling here, and
// a host wants that to be a clear parse error rather than a mystery.
$sandbox = new Sandbox();

foreach ([
	'for-var is const' => 'for i = 1, 3 do i = i * 10 end',
	'<const> is const' => 'local x <const> = 1 x = 2',
	// A goto may not jump into a local's scope -- EXCEPT to a label at the end
	// of a block, where the local is going out of scope anyway. That exemption
	// is what makes the `::continue::` idiom above legal, so both halves are
	// pinned: without it the first of these would be an error too.
	'goto to end-of-block label' => 'goto skip local x = 1 ::skip::',
	'goto into a local scope' => 'do goto skip local x = 1 ::skip:: x = 2 end',
	'goto to no label' => 'goto nowhere',
	'break outside loop' => 'break',
] as $label => $source) {
	$result = $sandbox->validate($source, '=rule');
	printf("%s = %s\n", $label, $result->valid ? 'compiles' : $result->message);
}

$sandbox->close();

?>
--EXPECT--
int subtype = "integer", "float", nil
3 == 3.0 = true
3 // 1 type = "integer", "float"
7 / 2 = 3.5
7 // 2 = 3
7.0 // 2 = 3.0
-7 // 2 = -4
7 // -2 = -4
-7 // -2 = 3
-7 % 3 = 2
7 % -3 = -2
-7 % -3 = -1
5.5 % 2 = 1.5
-5.5 % 2 = 0.5
maxint + 1 = true
minint - 1 = true
maxint = 9223372036854775807
minint = -9223372036854775808
1 // 0 = "! attempt to divide by zero"
1 % 0 = "! attempt to perform 'n%0'"
1.0 // 0 = inf
1 / 0 = inf
-1 / 0 = -inf
0 / 0 ~= itself = true
bits = 255, 15, 240, -1, 16, 16
shift over width = 0, 1
3.5 | 1 = "! number has no integer representation"
3.0 | 1 = 3
2^10 = 1024.0, "float"
"10" + 5 = 15, "integer"
"0x10" + 0 = 16
"1e2" + 0 = 100.0, "float"
"ten" + 1 = "! attempt to add a 'string' with a 'number'"
"1" < 2 = "! attempt to compare string with number"
10 .. 20 = "1020"
1.0 .. "" = "1.0"
tonumber = 12, 255, 35, 2
tonumber fail = nil, nil, nil
tonumber base 37 = "! bad argument #2 to '?' (base out of range)"
# = 5, 3
truthy = true, true, false, false
three() = 1, 2, 3
(three()) = 1
in middle = 1, "after"
assignment = 1, 2, 3, nil
table.pack = 3
in table = 3, 2
select # = 0, 1, 3
select 2 = "b", "c"
select -1 = "c"
select 0 = "! bad argument #1 to 'select' (index out of range)"
varargs = 3, "x", nil, "z"
shared upvalue = 2
per-iteration = 1, 2, 3
goto continue = {1, 3, 5}
float step = {1.0, 1.5, 2.0}
negative step = {3, 2, 1}
zero step = "! 'for' step is zero"
empty range = 0
for-var is const = rule:1: attempt to assign to const variable 'i'
<const> is const = rule:1: attempt to assign to const variable 'x'
goto to end-of-block label = compiles
goto into a local scope = rule:1: <goto skip> at line 1 jumps into the scope of 'x'
goto to no label = rule:1: no visible label 'nowhere' for <goto> at line 1
break outside loop = rule:1: break outside loop near 'break'
