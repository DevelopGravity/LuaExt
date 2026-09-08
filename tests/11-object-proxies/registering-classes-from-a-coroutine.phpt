--TEST--
registerClass() and unregister() from a coroutine's host callback use the running state
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\LuaMethod;
use DevelopGravity\LuaExt\Sandbox;

final class LateClock
{
	#[LuaMethod]
	public static function now(): int { return 42; }
}

// The same shape registering-from-a-coroutine-uses-the-running-state.phpt
// pins for registerLibrary(): the class-table plant and the unregister clear
// both push onto "the state that is executing", which inside a coroutine's
// host callback is the coroutine, not the main thread.
$sandbox = new Sandbox();

$sandbox->registerLibrary('host', [
	'install' => static function () use (&$sandbox): void {
		$sandbox->registerClass(LateClock::class, luaName: 'clock');
	},
	'retire' => static function () use (&$sandbox): void {
		$sandbox->unregister('clock');
	},
]);

[$ok, $value] = $sandbox->eval(<<<'LUA'
	local co = coroutine.create(function()
		host.install()
		return clock.now()
	end)
	local ok, value = coroutine.resume(co)
	return ok, value
LUA, '=co');

printf("inside the coroutine: %s / %d\n", var_export($ok, true), $value);

// Still planted after the call that installed it ended.
printf("in a later call:      %d\n", $sandbox->eval('return clock.now()', '=later')[0]);

// And retired from inside a coroutine, gone for good.
[$ok] = $sandbox->eval(<<<'LUA'
	local co = coroutine.create(function()
		host.retire()
		return true
	end)
	local ok = select(2, coroutine.resume(co))
	return ok
LUA, '=retire');

printf("retired inside:       %s\n", var_export($ok, true));
printf("cleared after:        %s\n", $sandbox->eval('return type(clock)', '=gone')[0]);

$sandbox->close();

?>
--EXPECT--
inside the coroutine: true / 42
in a later call:      42
retired inside:       true
cleared after:        nil
