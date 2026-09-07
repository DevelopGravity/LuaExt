--TEST--
registerLibrary() called from a host callback inside a coroutine builds on the running state
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Sandbox;
use DevelopGravity\LuaExt\SandboxConfig;

// Every host entry that runs Lua resolves "the state that is executing" --
// registerLibrary()'s table-building pcall used to hardcode the main thread,
// so a registration made from inside a coroutine's host callback ran on a
// stack whose C-call budget lua_resume had lent to the coroutine. The library
// must land, be reachable from the coroutine that asked for it, and survive
// into later calls.

$sandbox = new Sandbox(new SandboxConfig());

$sandbox->registerLibrary('host', [
	'install' => static function () use (&$sandbox): void {
		$sandbox->registerLibrary('late', [
			'greet' => static fn (): string => 'late ok',
		]);
	},
]);

[$ok, $value] = $sandbox->eval(<<<'LUA'
	local co = coroutine.create(function()
		host.install()
		return late.greet()
	end)
	local ok, value = coroutine.resume(co)
	return ok, value
LUA, '=co');

printf("inside the coroutine: %s / %s\n", var_export($ok, true), $value);

// Still there after the call that installed it ended.
printf("in a later call:      %s\n", $sandbox->eval('return late.greet()', '=later')[0]);

$sandbox->close();

?>
--EXPECT--
inside the coroutine: true / late ok
in a later call:      late ok
