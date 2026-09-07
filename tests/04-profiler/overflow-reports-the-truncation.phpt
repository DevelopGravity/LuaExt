--TEST--
A profile that overflows the function table says so instead of silently dropping the tail
--EXTENSIONS--
luaext
--SKIPIF--
<?php
// Sampling needs the count hook; see sampling-attributes-cost.phpt.
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

// The counts table stops at 4096 distinct functions; past it, new functions
// set the overflow flag and getProfile() appends a synthetic truncation
// entry. A profile silently missing its most expensive function would be
// actively misleading, which is why the flag exists -- and why this test
// exists: nothing ever pushed the table past the cap.
//
// 4300 distinct identities, one function per source LINE of one chunk, each
// busy long enough that a sample must land on it while it runs.
$lines = ['local fns = {}'];

for ($index = 1; $index <= 4300; $index++) {
	$lines[] = sprintf(
		'fns[%d] = function () local x = 0 for i = 1, 60 do x = x + i end return x end',
		$index,
	);
}

$lines[] = 'makers = fns';

$sandbox = new Sandbox(new SandboxConfig(
	limits: new Limits(cpuSeconds: 30.0, wallClockSeconds: 60.0, maxSourceBytes: 1048576),
));

$sandbox->setGlobal('makers', []);
(void) $sandbox->eval(implode("\n", $lines), '=forest');

var_dump($sandbox->enableProfiler(0.000001));

(void) $sandbox->eval('for index = 1, #makers do makers[index]() end', '=burn');

$sandbox->disableProfiler();

$marker = '[truncated: more than 4096 functions sampled]';

foreach ([ProfilerUnit::Samples, ProfilerUnit::Percent, ProfilerUnit::Seconds] as $unit) {
	$profile = $sandbox->getProfile($unit);

	printf(
		"%-7s entries<=%d: %s, truncation marker: %s\n",
		$unit->name,
		4097,
		var_export(count($profile) <= 4097, true),
		var_export(array_key_exists($marker, $profile), true),
	);
}

$sandbox->close();

?>
--EXPECT--
bool(true)
Samples entries<=4097: true, truncation marker: true
Percent entries<=4097: true, truncation marker: true
Seconds entries<=4097: true, truncation marker: true
