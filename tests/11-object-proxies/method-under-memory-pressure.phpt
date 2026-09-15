--TEST--
An instance method hitting the memory ceiling aborts cleanly and the sandbox recovers
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

// The sibling test pins .new; this drives the structurally different path —
// a plain method call marshalling an argument and auto-wrapping a returned
// object into a fresh proxy — into the same ceiling.
final class Mint
{
	#[LuaMethod]
	public function __construct(public readonly int $value = 0) {}

	#[LuaMethod]
	public function next(string $tag): Mint { return new Mint($this->value + strlen($tag)); }

	#[LuaMethod]
	public function value(): int { return $this->value; }
}

$sandbox = new Sandbox(new SandboxConfig(
	limits: new Limits(memoryBytes: 262144, cpuSeconds: 5.0, wallClockSeconds: 10.0),
));
$sandbox->registerClass(Mint::class);

// Call methods until the budget refuses one mid-dispatch — during the
// argument conversion or the wrap of the returned instance. The refusal must
// be the fatal MemoryLimitError, never a crash or a half-wrapped return.
try {
	(void) $sandbox->eval(<<<'LUA'
		local m = Mint.new(0)
		local keep = {}
		while true do
			keep[#keep + 1] = m:next(string.rep('x', 64))
		end
	LUA);
	echo "NOT STOPPED\n";
} catch (MemoryLimitError) {
	echo "stopped by the memory ceiling\n";
}

// The sandbox is intact: raise the ceiling and dispatch works again.
$sandbox->setLimits($sandbox->limits()->with(memoryBytes: 33554432));

// A refusal mid-dispatch must not strand the receiver or a half-wrapped
// return in the live list: once the refused run is unreachable the counter
// comes all the way back.
(void) $sandbox->eval('collectgarbage("collect")');
var_dump($sandbox->stats()->liveObjectProxies);

var_dump($sandbox->eval("return Mint.new(0):next('abc'):value()")[0]);

(void) $sandbox->eval('collectgarbage("collect")');
var_dump($sandbox->stats()->liveObjectProxies);

$sandbox->close();

?>
--EXPECT--
stopped by the memory ceiling
int(0)
int(3)
int(0)
