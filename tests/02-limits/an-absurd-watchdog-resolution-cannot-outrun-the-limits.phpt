--TEST--
luaext.watchdog_resolution_us is clamped, so no configured floor can outrun the limits
--EXTENSIONS--
luaext
--INI--
luaext.watchdog_resolution_us=9223372036854775807
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Exception\WallClockLimitError;
use DevelopGravity\LuaExt\Limits;
use DevelopGravity\LuaExt\Sandbox;
use DevelopGravity\LuaExt\SandboxConfig;

// The resolution knob used to go through the stock OnUpdateLong and a raw
// multiply by 1000 into a uint64_t: PHP_INT_MAX wrapped into a ~584-year
// wake-up floor, and the watchdog thread simply never woke to deliver a
// limit. The knob is a floor on delivery LATENCY -- it must never be able to
// dwarf the limits it delivers -- so it is clamped to one second at the INI
// and again inside the watchdog. This file configures the wrapping value and
// proves a wall-clock limit still stops an unbounded loop.

$sandbox = new Sandbox(new SandboxConfig(
	limits: (new Limits())->with(cpuSeconds: null, wallClockSeconds: 0.1),
));

try {
	(void) $sandbox->eval('while true do end', '=spin');
	print "returned\n";
} catch (WallClockLimitError $error) {
	print "stopped by the wall-clock limit\n";
}

$sandbox->close();

?>
--EXPECT--
stopped by the wall-clock limit
