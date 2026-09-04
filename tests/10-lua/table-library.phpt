--TEST--
Lua conformance: the table library, including move and sort which this build patched
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

require __DIR__ . '/conformance.inc';

// table.move, table.sort and table.concat are three of the places LUAEXT_CHECK
// was inserted so a long-running C loop can be interrupted -- the older
// luasandbox banned table.move outright for want of one. Those insertions sit
// inside the loops that do the copying and the comparing, so a misplaced check
// changes results rather than failing loudly.

conformance(<<<'LUA'
	-- insert and remove, at the end and in the middle.
	local list = {'a', 'b', 'c'}
	table.insert(list, 'd')
	table.insert(list, 1, 'z')
	row('insert', list)
	row('remove end', table.remove(list), list)
	row('remove front', table.remove(list, 1), list)
	row('remove from empty', table.remove({}))

	-- The position must be within the sequence: insert may go one past the end,
	-- and anything further is refused rather than leaving a hole.
	try('insert past end', table.insert, {1, 2}, 5, 'x')
	try('insert at zero', table.insert, {1, 2}, 0, 'x')
	try('insert wrong arity', table.insert, {1, 2}, 1, 2, 3)

	-- concat, with a separator and with a range. Only strings and numbers.
	row('concat', table.concat({1, 2, 3}), table.concat({1, 2, 3}, '-'))
	row('concat range', table.concat({'a', 'b', 'c', 'd'}, ',', 2, 3))
	row('concat empty', table.concat({}), table.concat({1, 2}, ',', 2, 1))
	try('concat a table', table.concat, {{}})
	try('concat a boolean', table.concat, {true})

	-- move, including the overlapping cases, which are the ones a naive
	-- implementation gets wrong.
	row('move', table.move({1, 2, 3}, 1, 3, 2))
	row('move to another', table.move({1, 2, 3}, 1, 3, 1, {}))
	row('move overlapping up', table.move({1, 2, 3, 4, 5}, 1, 3, 2))
	row('move overlapping down', table.move({1, 2, 3, 4, 5}, 2, 4, 1))
	row('move empty range', table.move({1, 2, 3}, 3, 1, 1))
	row('move single', table.move({1, 2, 3}, 2, 2, 1))

	-- sort: default order, a custom comparator, and the refusals. An invalid
	-- comparator is DETECTED rather than being allowed to read out of bounds.
	local numbers = {5, 2, 8, 1, 9}
	table.sort(numbers)
	row('sort', numbers)

	local words = {'pear', 'apple', 'fig'}
	table.sort(words, function (a, b) return #a < #b end)
	row('sort by length', words)

	table.sort(words, function (a, b) return a > b end)
	row('sort reverse', words)

	row('sort empty', (function () local t = {} table.sort(t) return t end)())
	row('sort one', (function () local t = {'x'} table.sort(t) return t end)())

	try('sort mixed types', table.sort, {1, 'a', {}})
	try('sort invalid order', table.sort, {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12},
		function () return true end)

	-- pack and unpack, including the n field and an explicit range.
	local packed = table.pack('a', nil, 'c')
	row('pack', packed.n, packed[1], packed[2], packed[3])
	row('unpack', table.unpack({1, 2, 3}))
	row('unpack range', table.unpack({1, 2, 3, 4}, 2, 3))
	row('unpack past end', table.unpack({1, 2}, 1, 4))
	row('unpack empty', select('#', table.unpack({}, 1, 0)))

	-- The border (#) on a table with holes is ANY border, not the largest -- so
	-- these assert the property rather than a specific number.
	local holed = {1, 2, nil, 4}
	local border = #holed
	row('border is a border', border == 2 or border == 4)
	row('sequence length', #{1, 2, 3}, #{}, #{nil})

	-- A nil value removes a key; it does not store a nil.
	local removable = {a = 1}
	removable.a = nil
	row('nil removes', removable.a, next(removable))

	-- nil and NaN keys are refused, because neither can be looked up again.
	try('nil key', function () local t = {} t[nil] = 1 end)
	try('nan key', function () local t = {} t[0 / 0] = 1 end)

	-- A float key that is exactly integral normalises to the integer key, so
	-- t[2] and t[2.0] are the same slot.
	local keyed = {}
	keyed[2.0] = 'two'
	row('float key normalises', keyed[2], rawget(keyed, 2))
	LUA);

?>
--EXPECT--
insert = {"z", "a", "b", "c", "d"}
remove end = "d", {"z", "a", "b", "c"}
remove front = "z", {"a", "b", "c"}
remove from empty = nil
insert past end = "! bad argument #2 to '?' (position out of bounds)"
insert at zero = "! bad argument #2 to '?' (position out of bounds)"
insert wrong arity = "! wrong number of arguments to 'insert'"
concat = "123", "1-2-3"
concat range = "b,c"
concat empty = "", ""
concat a table = "! invalid value (table) at index 1 in table for 'concat'"
concat a boolean = "! invalid value (boolean) at index 1 in table for 'concat'"
move = {1, 1, 2, 3}
move to another = {1, 2, 3}
move overlapping up = {1, 1, 2, 3, 5}
move overlapping down = {2, 3, 4, 4, 5}
move empty range = {1, 2, 3}
move single = {2, 2, 3}
sort = {1, 2, 5, 8, 9}
sort by length = {"fig", "pear", "apple"}
sort reverse = {"pear", "fig", "apple"}
sort empty = {}
sort one = {"x"}
sort mixed types = "! attempt to compare table with number"
sort invalid order = "! invalid order function for sorting"
pack = 3, "a", nil, "c"
unpack = 1, 2, 3
unpack range = 2, 3
unpack past end = 1, 2, nil, nil
unpack empty = 0
border is a border = true
sequence length = 3, 0, 0
nil removes = nil, nil
nil key = "! table index is nil"
nan key = "! table index is NaN"
float key normalises = "two", "two"
