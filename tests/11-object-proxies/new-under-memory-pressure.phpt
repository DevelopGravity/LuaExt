--TEST--
.new hitting the memory ceiling aborts cleanly and the sandbox recovers
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Exception\MemoryLimitError;
use DevelopGravity\LuaExt\Limits;
use DevelopGravity\LuaExt\LuaMethod;
use DevelopGravity\LuaExt\Sandbox;
use DevelopGravity\LuaExt\SandboxConfig;

final class Pebble
{
	#[LuaMethod]
	public function __construct() {}

	#[LuaMethod]
	public function id(): int { return 1; }
}

$sandbox = new Sandbox(new SandboxConfig(
	limits: new Limits(memoryBytes: 262144, cpuSeconds: 5.0, wallClockSeconds: 10.0),
));
$sandbox->registerClass(Pebble::class);

// Mint proxies until the budget refuses one mid-construction. The refusal must
// be the fatal MemoryLimitError -- never a crash, never a catchable leak of a
// half-made object.
try {
	(void) $sandbox->eval(<<<'LUA'
		local keep = {}
		while true do
			keep[#keep + 1] = Pebble.new()
		end
	LUA);
	echo "NOT STOPPED\n";
} catch (MemoryLimitError) {
	echo "stopped by the memory ceiling\n";
}

// The sandbox is intact: raise the ceiling and it works again.
$sandbox->setLimits($sandbox->limits()->with(memoryBytes: 33554432));

// Everything the refused run minted is unreachable now, so the counter must
// come all the way back. A proxy the refusal stranded in the live list -- one
// bound to a count that was incremented but never handed back -- would hold it
// above zero for the rest of the sandbox's life.
(void) $sandbox->eval('collectgarbage("collect")');
var_dump($sandbox->stats()->liveObjectProxies);

var_dump($sandbox->eval('return Pebble.new():id()')[0]);

(void) $sandbox->eval('collectgarbage("collect")');
var_dump($sandbox->stats()->liveObjectProxies);

$sandbox->close();

?>
--EXPECT--
stopped by the memory ceiling
int(0)
int(1)
int(0)
