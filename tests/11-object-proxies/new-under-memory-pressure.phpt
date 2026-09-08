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
var_dump($sandbox->eval('return Pebble.new():id()')[0]);

$sandbox->close();

?>
--EXPECT--
stopped by the memory ceiling
int(1)
