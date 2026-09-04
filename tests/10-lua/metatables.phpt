--TEST--
Lua conformance: every metamethod fires, and fires on the right operand
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

require __DIR__ . '/conformance.inc';

// Metamethod dispatch is the part of the VM most entangled with the interrupt
// checks: luaT_trybinTM and friends are called from the same opcodes lvm.c was
// patched in. A metamethod that stopped firing, or started firing on the wrong
// operand, would break every host library built on __index without breaking
// anything the rest of this suite tests.

conformance(<<<'LUA'
	-- __index as a table and as a function, including the chain.
	local base = {inherited = 'from base'}
	local middle = setmetatable({}, {__index = base})
	local leaf = setmetatable({own = 'mine'}, {__index = middle})
	row('__index chain', leaf.own, leaf.inherited, leaf.missing)

	local computed = setmetatable({}, {__index = function (t, k) return 'computed:' .. k end})
	row('__index function', computed.anything)

	-- __newindex diverts only assignments to ABSENT keys.
	local log = {}
	local guarded = setmetatable({present = 1}, {
		__newindex = function (t, k, v) log[#log + 1] = k rawset(t, k, v) end,
	})
	guarded.present = 2
	guarded.fresh = 3
	row('__newindex', log, guarded.present, guarded.fresh)

	-- raw* bypass the metatable entirely.
	row('raw access', rawget(leaf, 'inherited'), rawlen({1, 2, 3}), rawequal(leaf, leaf))

	-- Arithmetic, including the reversed case: a metamethod fires when EITHER
	-- operand carries it, and gets the operands in source order.
	local mt = {}
	local function box(n) return setmetatable({n = n}, mt) end
	mt.__add = function (a, b)
		local an = type(a) == 'table' and a.n or a
		local bn = type(b) == 'table' and b.n or b
		return an .. '+' .. bn
	end
	mt.__sub = function (a, b) return 'sub' end
	mt.__mul = function (a, b) return 'mul' end
	mt.__div = function (a, b) return 'div' end
	mt.__mod = function (a, b) return 'mod' end
	mt.__pow = function (a, b) return 'pow' end
	mt.__unm = function (a) return 'unm' end
	mt.__idiv = function (a, b) return 'idiv' end
	mt.__band = function (a, b) return 'band' end
	mt.__bor = function (a, b) return 'bor' end
	mt.__bxor = function (a, b) return 'bxor' end
	mt.__bnot = function (a) return 'bnot' end
	mt.__shl = function (a, b) return 'shl' end
	mt.__shr = function (a, b) return 'shr' end
	mt.__concat = function (a, b) return 'concat' end
	mt.__len = function (a) return 99 end
	mt.__call = function (self, x) return 'called:' .. x end
	mt.__tostring = function (self) return 'boxed(' .. self.n .. ')' end

	row('__add order', box(1) + box(2), box(1) + 5, 5 + box(1))
	row('arith', box(1) - box(2), box(1) * 1, box(1) / 1, box(1) % 1, box(1) ^ 1, -box(1))
	row('idiv', box(1) // 1)
	row('bitwise', box(1) & 1, box(1) | 1, box(1) ~ 1, ~box(1), box(1) << 1, box(1) >> 1)
	row('__concat', box(1) .. 'x', 'x' .. box(1))
	row('__len', #box(1))
	row('__call', box(1)('arg'))
	row('__tostring', tostring(box(7)))

	-- __eq fires ONLY when both operands are tables (or both userdata) and they
	-- are not primitively equal. It never fires for a table against a number.
	local eqcalls = 0
	local eqmt = {__eq = function (a, b) eqcalls = eqcalls + 1 return true end}
	local p, q = setmetatable({}, eqmt), setmetatable({}, eqmt)
	row('__eq different', p == q, eqcalls)
	eqcalls = 0
	row('__eq identical', p == p, eqcalls)
	eqcalls = 0
	row('__eq vs number', p == 1, eqcalls)

	-- __lt and __le are separate; 5.4 removed the "not (b < a)" fallback for
	-- __le, so a metatable with only __lt does NOT get <= for free.
	local ltonly = setmetatable({}, {__lt = function (a, b) return true end})
	local other = setmetatable({}, getmetatable(ltonly))
	row('__lt', ltonly < other)
	try('__le without it', function () return ltonly <= other end)

	local both = {__lt = function () return true end, __le = function () return false end}
	local r, s = setmetatable({}, both), setmetatable({}, both)
	row('__lt and __le', r < s, r <= s)

	-- __index on a string is the string library, which is how ("x"):upper()
	-- works at all. Assigning through it must not be possible.
	row('string __index', ('abc'):upper(), ('abc').len)
	row('string metatable', type(getmetatable('')), getmetatable('').__index == string)

	-- __metatable makes the metatable unreadable and unreplaceable, which is the
	-- mechanism this extension uses to protect its own error values.
	local sealed = setmetatable({}, {__metatable = 'locked'})
	row('__metatable', getmetatable(sealed))
	try('setmetatable on sealed', setmetatable, sealed, {})

	-- __name only affects the default tostring, and this build's tostring has
	-- had the address removed -- so it shows the name and nothing else.
	local named = setmetatable({}, {__name = 'MyType'})
	row('__name is not an address', (tostring(named):find('0x')) == nil)

	-- Absent metamethods still produce the ordinary type errors.
	try('add a table', function () return {} + 1 end)
	try('concat a table', function () return {} .. 'x' end)
	try('index a number', function () return (5).missing end)
	try('call a table', function () return ({})() end)
	LUA);

?>
--EXPECT--
__index chain = "mine", "from base", nil
__index function = "computed:anything"
__newindex = {"fresh"}, 2, 3
raw access = nil, 3, true
__add order = "1+2", "1+5", "5+1"
arith = "sub", "mul", "div", "mod", "pow", "unm"
idiv = "idiv"
bitwise = "band", "bor", "bxor", "bnot", "shl", "shr"
__concat = "concat", "concat"
__len = 99
__call = "called:arg"
__tostring = "boxed(7)"
__eq different = true, 1
__eq identical = true, 0
__eq vs number = false, 0
__lt = true
__le without it = "! attempt to compare two table values"
__lt and __le = true, false
string __index = "ABC", <function>
string metatable = "table", true
__metatable = "locked"
setmetatable on sealed = "! cannot change a protected metatable"
__name is not an address = true
add a table = "! attempt to perform arithmetic on a table value"
concat a table = "! attempt to concatenate a table value"
index a number = "! attempt to index a number value"
call a table = "! attempt to call a table value"
