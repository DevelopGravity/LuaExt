--TEST--
getProfile(ProfilerUnit::Seconds) distributes the sampled window, not the lifetime
--EXTENSIONS--
luaext
--SKIPIF--
<?php
// Same reason as sampling-attributes-cost.phpt: no samples can be taken on a
// build whose count hook is the CPU limit.
use DevelopGravity\LuaExt\LimitSupport;
use DevelopGravity\LuaExt\Sandbox;

if (Sandbox::features()['cpuLimit'] === LimitSupport::Unsupported) {
	echo "skip this build reports LimitSupport::Unsupported for the CPU limit";
}
?>
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Limits;
use DevelopGravity\LuaExt\ProfilerUnit;
use DevelopGravity\LuaExt\Sandbox;
use DevelopGravity\LuaExt\SandboxConfig;

$sandbox = new Sandbox(new SandboxConfig(
	limits: new Limits(cpuSeconds: 30.0, wallClockSeconds: 60.0),
));

// Burn a clearly larger amount of CPU with the profiler OFF. Whole-lifetime
// scaling used to hand every one of these seconds to whatever was later
// sampled; window scaling must never see them.
(void) $sandbox->eval('local x = 0 for i = 1, 8000000 do x = x + i * 0.5 end return x', '=burn');
$burned = $sandbox->stats()->cpuSeconds;

var_dump($sandbox->enableProfiler(0.0005));
$windowStart = $sandbox->stats()->cpuSeconds;
(void) $sandbox->eval('local x = 0 for i = 1, 1000000 do x = x + i * 0.5 end return x', '=sampled');
$windowEnd = $sandbox->stats()->cpuSeconds;
$sandbox->disableProfiler();

$seconds = $sandbox->getProfile(ProfilerUnit::Seconds);
$attributed = array_sum($seconds);

// The attributed total covers the sampled window (small tolerance for the
// enable/disable edges), and stays well under the pre-enable burn -- the
// burn did eight times the sampled work, so lifetime scaling cannot pass.
printf("window-bounded: %s\n", var_export(
	$attributed <= ($windowEnd - $windowStart) + 0.05,
	true,
));
printf("excludes burn:  %s\n", var_export($attributed < $burned, true));
printf("positive:       %s\n", var_export($attributed > 0.0, true));

$sandbox->close();

?>
--EXPECT--
bool(true)
window-bounded: true
excludes burn:  true
positive:       true
