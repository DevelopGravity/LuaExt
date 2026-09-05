--TEST--
A script cannot take the process down, whichever way it reaches for the C stack
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Exception\LuaThrowable;
use DevelopGravity\LuaExt\Sandbox;

// Every other adversarial test here asks whether a script can ESCAPE something.
// This one asks whether it can simply kill the host, which is a different
// question with a different failure mode: not a wrong answer, a signal.
//
// The routes are the ones that reach C recursion or C-level allocation without
// going through anything the sandbox meters directly -- a metamethod that calls
// itself is a C frame per hop, and no Lua-level limit counts those. Lua's own
// LUAI_MAXCCALLS guard is what stops most of them, which is precisely why this
// file exists: that guard is upstream's, it is load-bearing for us, and nothing
// else here would notice if a patch or a build flag disturbed it.
//
// WHAT IS ASSERTED IS THE PROPERTY, NOT THE MESSAGE. Which limit fires first is
// legitimately machine-dependent -- unbounded tail recursion spends CPU without
// growing the C stack, so it lands on the CPU limit on one box and elsewhere on
// another. "It stopped, and we are still here" is the claim that must hold
// everywhere; anything narrower would be a test of this laptop.

$attempts = [
	// Deep C recursion, reached five different ways. Each hop is a C frame.
	'plain recursion' => 'local function f(n) return f(n + 1) end return f(1)',
	'__index recursion' => 'local t = setmetatable({}, {__index = function (s, k) return s[k] end}) return t.x',
	'__tostring recursion' => 'local t = setmetatable({}, {__tostring = function (s) return tostring(s) end}) return tostring(t)',
	'__concat recursion' => 'local t = setmetatable({}, {__concat = function (a, b) return a .. b end}) return t .. t',
	'__eq recursion' => 'local mt = {} mt.__eq = function (a, b) return a == b end
		local a, b = setmetatable({}, mt), setmetatable({}, mt) return a == b',

	// Allocation the Lua heap has to serve, at a size no budget would allow.
	'enormous string' => 'return string.rep("x", 1e9)',
	'enormous table' => 'local t = {} for i = 1, 1e9 do t[i] = i end return #t',

	// Nesting, of the kinds that have their own caps.
	'coroutine nesting' => 'local function f(n) if n <= 0 then return 1 end
		local co = coroutine.create(f) local ok, v = coroutine.resume(co, n - 1) return v end
		return f(10000)',
	'pcall nesting' => 'local function f(n) if n > 0 then return pcall(f, n - 1) end return 1 end
		return f(50000)',

	// A deeply nested value handed to something that has to walk it.
	'deep error value' => 'local t = {} local c = t
		for _ = 1, 20000 do c.n = {} c = c.n end error(t)',
	'deep table build' => 'local t = {} local c = t
		for _ = 1, 20000 do c[1] = {} c = c[1] end return 1',

	// Argument extremes at the C boundary.
	'select past the end' => 'return select(2 ^ 31, 1, 2, 3)',
	// Wrapped in pcall on purpose, and it still stops: a MemoryLimitError is
	// fatal, and fatal errors are not pcall's to catch. That is the whole
	// contract of tests/03-adversarial/fatal-error-uncatchable.phpt, restated
	// here because a script reaching for a crash reaches for pcall too.
	'huge string.rep in pcall' => 'return pcall(string.rep, "x", 1e9, "sep")',
];

foreach ($attempts as $label => $source) {
	$sandbox = new Sandbox();

	try {
		(void) $sandbox->eval($source, '=crash');
		$outcome = 'returned';
	} catch (LuaThrowable $error) {
		// The class is not asserted: which budget a given machine reaches first
		// is not a property of this extension.
		$outcome = 'stopped';
	}

	printf("%-28s %s\n", $label, $outcome);

	$sandbox->close();
}

// The claim that matters: after all of that, the extension still works. A
// process that survived by being left in a broken state has not survived.
$survivor = new Sandbox();
var_dump($survivor->eval('return 6 * 7')[0]);
$survivor->close();

?>
--EXPECT--
plain recursion              stopped
__index recursion            stopped
__tostring recursion         stopped
__concat recursion           stopped
__eq recursion               stopped
enormous string              stopped
enormous table               stopped
coroutine nesting            stopped
pcall nesting                returned
deep error value             stopped
deep table build             returned
select past the end          returned
huge string.rep in pcall     stopped
int(42)
