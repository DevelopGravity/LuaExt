--TEST--
Lua conformance: what iteration guarantees, and what it deliberately does not
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

require __DIR__ . '/conformance.inc';

// Iteration order over a table's hash part is UNSPECIFIED, and in this build it
// is also seeded per sandbox from a CSPRNG -- that is what defeats hash-flood
// denial of service (see SECURITY.md). So nothing here may assert an order for
// pairs(); what it asserts is the set, the count, and the guarantees Lua does
// make. A test that accidentally depended on hash order would pass locally and
// fail on another machine, or on the same machine tomorrow.

conformance(<<<'LUA'
	-- ipairs stops at the first nil, and that IS guaranteed.
	local holed = {1, 2, nil, 4}
	local seen = {}
	for _, value in ipairs(holed) do seen[#seen + 1] = value end
	row('ipairs stops at nil', seen)

	local dense = {'a', 'b', 'c'}
	local indexed = {}
	for index, value in ipairs(dense) do indexed[#indexed + 1] = index .. value end
	row('ipairs order', indexed)
	row('ipairs of empty', (function ()
		local n = 0
		for _ in ipairs({}) do n = n + 1 end
		return n
	end)())

	-- pairs visits every key exactly once. The SET is guaranteed; the order is
	-- not, so the keys are sorted before being shown.
	local mixed = {x = 1, y = 2, z = 3, [1] = 'one', [2] = 'two'}
	local keys, count = {}, 0
	for key in pairs(mixed) do
		keys[#keys + 1] = tostring(key)
		count = count + 1
	end
	table.sort(keys)
	row('pairs visits all', count, keys)

	-- next is what pairs is built on, and next(t) with no second argument
	-- starts the walk. next past the end returns nil.
	local single = {only = 'value'}
	local key, value = next(single)
	row('next', key, value, next(single, key))
	row('next of empty', next({}))

	-- A generic for takes any iterator triple, which is how a host-provided
	-- iterator plugs in.
	local function range(n)
		local i = 0
		return function ()
			i = i + 1
			if i <= n then return i, i * i end
		end
	end
	local squares = {}
	for _, square in range(4) do squares[#squares + 1] = square end
	row('custom iterator', squares)

	-- The stateless iterator protocol: f, state, control. next itself is one.
	local stateless = {}
	for k, v in next, {alpha = 1} do stateless[#stateless + 1] = k .. v end
	row('stateless protocol', stateless)

	-- __pairs IS honoured -- it survived into 5.5 -- so a metatable can replace
	-- what pairs() walks entirely. A host handing a script a proxy table relies
	-- on this; a build that dropped it would iterate the raw table instead and
	-- leak whatever the proxy was hiding.
	local with_pairs_mt = setmetatable({real = 1}, {
		__pairs = function (t)
			local done = false
			return function ()
				if done then return nil end
				done = true
				return 'substituted', 'value'
			end, t, nil
		end,
	})
	local pairs_keys = {}
	for k in pairs(with_pairs_mt) do pairs_keys[#pairs_keys + 1] = tostring(k) end
	row('__pairs is honoured', pairs_keys)

	-- next() is the raw walk and is NOT redirected, which is how the extension's
	-- own stdlib audit walks a table a hostile __pairs cannot influence.
	local raw_keys = {}
	for k in next, with_pairs_mt do raw_keys[#raw_keys + 1] = tostring(k) end
	row('next ignores __pairs', raw_keys)

	-- pairs DOES honour __index for nothing at all: iteration is always raw.
	local inheriting = setmetatable({}, {__index = {inherited = 1}})
	local inherited_count = 0
	for _ in pairs(inheriting) do inherited_count = inherited_count + 1 end
	row('iteration is raw', inherited_count, inheriting.inherited)

	-- ASSIGNING to an existing field during a walk is allowed; ADDING a new one
	-- is undefined. Only the allowed case is exercised.
	local updatable = {a = 1, b = 2, c = 3}
	for k in pairs(updatable) do updatable[k] = 0 end
	row('assign during walk', updatable.a, updatable.b, updatable.c)

	-- Removing during a walk is allowed too, by assigning nil.
	local removable = {a = 1, b = 2, c = 3}
	for k in pairs(removable) do removable[k] = nil end
	row('remove during walk', next(removable))

	-- table.sort is not stable, so equal elements may come back in either
	-- order. What IS guaranteed is that the result is ordered by the
	-- comparator, which is what this checks.
	local records = {
		{name = 'b', rank = 1}, {name = 'a', rank = 2},
		{name = 'd', rank = 1}, {name = 'c', rank = 2},
	}
	table.sort(records, function (x, y) return x.rank < y.rank end)
	local ranks = {}
	for _, record in ipairs(records) do ranks[#ranks + 1] = record.rank end
	row('sorted by comparator', ranks)

	-- A total order over the whole array, checked as a property rather than as
	-- a specific permutation.
	local numbers = {}
	for index = 1, 50 do numbers[index] = (index * 37) % 50 end
	table.sort(numbers)
	local ordered = true
	for index = 2, #numbers do
		if numbers[index - 1] > numbers[index] then ordered = false end
	end
	row('sort is a total order', ordered, numbers[1], numbers[#numbers])
	LUA);

?>
--EXPECT--
ipairs stops at nil = {1, 2}
ipairs order = {"1a", "2b", "3c"}
ipairs of empty = 0
pairs visits all = 5, {"1", "2", "x", "y", "z"}
next = "only", "value", nil
next of empty = nil
custom iterator = {1, 4, 9, 16}
stateless protocol = {"alpha1"}
__pairs is honoured = {"substituted"}
next ignores __pairs = {"real"}
iteration is raw = 0, 1
assign during walk = 0, 0, 0
remove during walk = nil
sorted by comparator = {1, 1, 2, 2}
sort is a total order = true, 0, 49
