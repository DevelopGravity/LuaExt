--TEST--
A proxy's PHP destructor runs at the boundary drain, never inside Lua's collector
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\LuaMethod;
use DevelopGravity\LuaExt\Sandbox;

final class Tracked
{
	public static bool $destroyed = false;

	#[LuaMethod]
	public function id(): int { return 1; }

	public function __destruct()
	{
		self::$destroyed = true;
	}
}

$sandbox = new Sandbox();
$sandbox->registerClass(Tracked::class);
$sandbox->registerLibrary('probe', [
	'destroyed' => static fn (): bool => Tracked::$destroyed,
]);

$sandbox->setGlobal('t', new Tracked());

// Inside the call: collected, but the destructor has NOT run yet.
var_dump($sandbox->eval('t = nil collectgarbage("collect") return probe.destroyed()')[0]);

// The boundary drain ran when eval() returned.
var_dump(Tracked::$destroyed);
var_dump($sandbox->stats()->liveObjectProxies);

$sandbox->close();

?>
--EXPECT--
bool(false)
bool(true)
int(0)
