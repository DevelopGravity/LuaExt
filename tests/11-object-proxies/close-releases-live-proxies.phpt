--TEST--
close() finalizes wrapped objects that live proxies still hold, and out-of-scope equals close
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\LuaMethod;
use DevelopGravity\LuaExt\Sandbox;

final class Held
{
	public static int $destroyed = 0;

	#[LuaMethod]
	public function id(): int { return 1; }

	public function __destruct()
	{
		self::$destroyed++;
	}
}

// The single most common shutdown path: close() while a proxy is still
// referenced by a live Lua global nothing ever nilled or collected. The
// close-driven sweep must hand every wrapped object back.
$sandbox = new Sandbox();
$sandbox->registerClass(Held::class);
$sandbox->setGlobal('kept', new Held());
var_dump($sandbox->eval('return kept:id()')[0]);
var_dump($sandbox->stats()->liveObjectProxies);
var_dump(Held::$destroyed);

$sandbox->close();
var_dump(Held::$destroyed);

// The object simply falling out of scope runs the same teardown.
$scoped = new Sandbox();
$scoped->registerClass(Held::class);
$scoped->setGlobal('kept', new Held());
(void) $scoped->eval('return kept:id()');
unset($scoped);
var_dump(Held::$destroyed);

?>
--EXPECT--
int(1)
int(1)
int(0)
int(1)
int(2)
