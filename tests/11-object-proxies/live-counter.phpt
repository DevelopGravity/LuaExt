--TEST--
stats()->liveObjectProxies tracks proxies the interpreter still holds
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\LuaMethod;
use DevelopGravity\LuaExt\Sandbox;

final class Blip
{
	#[LuaMethod]
	public function id(): int { return 1; }
}

$sandbox = new Sandbox();
$sandbox->registerClass(Blip::class);

var_dump($sandbox->stats()->liveObjectProxies);

$sandbox->setGlobal('a', new Blip());
$sandbox->setGlobal('b', new Blip());
var_dump($sandbox->stats()->liveObjectProxies);

// Dropping one and collecting frees its proxy.
(void) $sandbox->eval('a = nil collectgarbage("collect")');
var_dump($sandbox->stats()->liveObjectProxies);

$sandbox->close();

?>
--EXPECT--
int(0)
int(2)
int(1)
