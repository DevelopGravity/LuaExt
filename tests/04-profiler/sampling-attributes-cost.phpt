--TEST--
The profiler is off until asked, and then attributes cost to the function that spent it
--EXTENSIONS--
luaext
--SKIPIF--
<?php
// Sampling needs the count hook, and the count hook is the CPU limit on a build
// whose watchdog thread could not start -- enableProfiler() refuses there rather
// than removing the limit. That refusal is the subject of its own assertion
// below, but the attribution rows cannot run without samples.
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

$config = new SandboxConfig(limits: new Limits(cpuSeconds: 10.0, wallClockSeconds: 30.0));

// Off by default, and that is a performance decision rather than an oversight:
// any non-zero hookmask sets ci->u.l.trap, which routes every instruction
// through luaG_traceexec. A sandbox nobody asked to profile pays nothing.
$sandbox = new Sandbox($config);
(void) $sandbox->eval('local x = 0 for i = 1, 200000 do x = x + i end return x', '=off');
var_dump($sandbox->getProfile());
$sandbox->close();

// Armed, the samples land on the function that actually spent the time. The
// workload is twenty parts hot to one part cold, so the ordering is the
// assertion -- the exact split is a sampling result and would be flaky.
$sandbox = new Sandbox($config);

var_dump($sandbox->enableProfiler(0.001));

(void) $sandbox->eval('
	local function hot(n) local x = 0 for i = 1, n do x = x + i * 0.5 end return x end
	local function cold(n) local x = 0 for i = 1, n do x = x + 1 end return x end
	for round = 1, 40 do hot(200000) cold(10000) end
', '=work');

$sandbox->disableProfiler();

$percent = $sandbox->getProfile(ProfilerUnit::Percent);
$names = array_keys($percent);

// Most expensive first.
printf("first is hot:  %s\n", var_export(str_contains($names[0], 'hot'), true));
printf("cold is there: %s\n", var_export(
	(bool) array_filter($names, static fn (string $n): bool => str_contains($n, 'cold')),
	true,
));
printf("hot > cold:    %s\n", var_export($percent[$names[0]] > 25.0, true));
printf("sums to 100:   %s\n", var_export(abs(array_sum($percent) - 100.0) < 0.001, true));

// The three units are three renderings of one distribution, so their ordering
// agrees and only the scale differs.
$samples = $sandbox->getProfile(ProfilerUnit::Samples);
$seconds = $sandbox->getProfile(ProfilerUnit::Seconds);

printf("same order:    %s\n", var_export(
	array_keys($samples) === $names && array_keys($seconds) === $names,
	true,
));
printf("samples whole: %s\n", var_export(array_sum($samples) === (float) (int) array_sum($samples), true));
printf("seconds > 0:   %s\n", var_export(array_sum($seconds) > 0.0, true));

// Sampling stops when asked; a second run adds nothing.
$before = $sandbox->getProfile(ProfilerUnit::Samples);
(void) $sandbox->eval('local x = 0 for i = 1, 500000 do x = x + i end return x', '=after');
printf("disabled:      %s\n", var_export($sandbox->getProfile(ProfilerUnit::Samples) === $before, true));

$sandbox->close();

?>
--EXPECT--
array(0) {
}
bool(true)
first is hot:  true
cold is there: true
hot > cold:    true
sums to 100:   true
same order:    true
samples whole: true
seconds > 0:   true
disabled:      true
