--TEST--
A host callback re-entering Lua from inside a coroutine uses the running state, not the main one
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\LuaFunction;
use DevelopGravity\LuaExt\Sandbox;
use DevelopGravity\LuaExt\SandboxConfig;

// A sandbox running a coroutine has two live lua_States, and a host callback
// invoked from inside one can push into Lua. Pushing onto the main thread while
// the index is resolved against the coroutine reads a slot that never held the
// value -- and with a callback taking no Lua arguments the resolved index lands
// at 0, which Lua's own API reads as the first free slot: whatever an earlier
// frame happened to leave there.
//
// Both halves of the boundary are exercised: LuaFunction::call() (a handle the
// host was given) and wrapCallable() (a closure the host builds mid-call). Both
// shapes are individually covered elsewhere -- callback-reentrancy.phpt for the
// re-entry, tests/05-coroutines/ for the coroutine -- so it is the combination
// that was untested, and the combination is where the states diverge.
//
// The callbacks deliberately take NO Lua arguments: that is the shape where a
// misresolved index leaves the API's own bounds rather than merely picking up
// the wrong live value.

$sandbox = new Sandbox(new SandboxConfig());

$sandbox->registerLibrary('host', [
	// Handed a Lua function, calls it back with no arguments of its own.
	'callBack' => static function (LuaFunction $target): int {
		[$result] = $target->call();

		return $result;
	},

	// Builds a closure mid-call and hands it to Lua to invoke.
	'makeAdder' => static function () use (&$sandbox): LuaFunction {
		return $sandbox->wrapCallable(
			static fn (int $left, int $right): int => $left + $right,
			'adder'
		);
	},
]);

// Outside a coroutine: the baseline both paths already satisfied.
[$plain] = $sandbox->eval('return host.callBack(function() return 41 end)', '=plain');
printf("main thread   => %d\n", $plain);

// Inside a coroutine, where running_L is the coroutine and sandbox->L is not.
[$viaCoroutine] = $sandbox->eval(<<<'LUA'
	local co = coroutine.wrap(function()
		coroutine.yield(host.callBack(function() return 41 end))
	end)

	return co()
	LUA, '=in-coroutine');
printf("in coroutine  => %d\n", $viaCoroutine);

// The same divergence on the wrapCallable side.
[$adderResult] = $sandbox->eval(<<<'LUA'
	local co = coroutine.wrap(function()
		local add = host.makeAdder()
		coroutine.yield(add(20, 22))
	end)

	return co()
	LUA, '=wrap-in-coroutine');
printf("wrapped       => %d\n", $adderResult);

// Nesting the two: a callback inside a coroutine inside a coroutine.
[$nested] = $sandbox->eval(<<<'LUA'
	local outer = coroutine.wrap(function()
		local inner = coroutine.wrap(function()
			coroutine.yield(host.callBack(function() return 7 end))
		end)
		coroutine.yield(inner())
	end)

	return outer()
	LUA, '=nested');
printf("nested        => %d\n", $nested);

// Repetition is the leak assertion: a value pushed onto the wrong state is
// never popped by the call that pushed it, so the main thread would grow one
// slot per crossing. A thousand crossings stays well inside LUAI_MAXSTACK only
// if nothing is being stranded.
[$looped] = $sandbox->eval(<<<'LUA'
	local total = 0

	for _ = 1, 1000 do
		local co = coroutine.wrap(function()
			coroutine.yield(host.callBack(function() return 1 end))
		end)
		total = total + co()
	end

	return total
	LUA, '=repeated');
printf("1000 crossings => %d\n", $looped);

$sandbox->close();

?>
--EXPECT--
main thread   => 41
in coroutine  => 41
wrapped       => 42
nested        => 7
1000 crossings => 1000
