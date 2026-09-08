--TEST--
debug.setmetatable stripping a live proxy hands its wrapped object back
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Capabilities;
use DevelopGravity\LuaExt\LuaMethod;
use DevelopGravity\LuaExt\Sandbox;
use DevelopGravity\LuaExt\SandboxConfig;

final class Held
{
	public static bool $destroyed = false;

	#[LuaMethod]
	public function id(): int { return 1; }

	public function __destruct()
	{
		self::$destroyed = true;
	}
}

// A userdata is finalised through the metatable it wears at collection time,
// so stripping a proxy's metatable under debugMutate detaches the __gc its
// PHP reference rides on -- the object used to leak for the whole request.
// The strip itself must settle the payload: reference deferred, live count
// down, destructor at the next boundary drain.
$mutate = new Sandbox(new SandboxConfig(
	capabilities: new Capabilities(debugMutate: true),
));
$mutate->registerClass(Held::class);
$mutate->setGlobal('s', new Held());

var_dump($mutate->stats()->liveObjectProxies);
(void) $mutate->eval('debug.setmetatable(s, {})');

// The boundary drain ran when eval() returned; nothing waits for close().
var_dump(Held::$destroyed);
var_dump($mutate->stats()->liveObjectProxies);

// The stripped shell is inert: still a userdata the script may hold, no
// longer one of ours.
var_dump($mutate->eval('return type(s)')[0]);

$mutate->close();

?>
--EXPECT--
int(1)
bool(true)
int(0)
string(8) "userdata"
