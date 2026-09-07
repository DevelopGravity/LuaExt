--TEST--
setLimits() refuses the debugHooks-plus-timing-limit pair the constructor refuses
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Capabilities;
use DevelopGravity\LuaExt\Exception\ConfigurationError;
use DevelopGravity\LuaExt\Limits;
use DevelopGravity\LuaExt\Sandbox;
use DevelopGravity\LuaExt\SandboxConfig;

// A sandbox granted debugHooks cannot also be bounded in time: the script can
// call debug.sethook() and displace the hook that turns the watchdog's flag
// into a stopped script. Construction refuses the pair -- but setLimits() is a
// second door into the same state, and it used to walk straight through,
// leaving a sandbox that reports a CPU limit no script has to respect.

// The constructor's refusal, as the baseline this must match.
try {
	new Sandbox(new SandboxConfig(
		capabilities: (new Capabilities())->with(debugHooks: true),
		limits: new Limits(cpuSeconds: 1.0),
	));
	echo "construct  => ACCEPTED\n";
} catch (ConfigurationError) {
	echo "construct  => ConfigurationError\n";
}

// A legal debugHooks sandbox: both timing limits explicitly cleared.
$sandbox = new Sandbox(new SandboxConfig(
	capabilities: (new Capabilities())->with(debugHooks: true),
	limits: new Limits(cpuSeconds: null, wallClockSeconds: null),
));

// Arming either limit afterwards must be refused the same way.
foreach ([
	'cpu ' => new Limits(cpuSeconds: 1.0, wallClockSeconds: null),
	'wall' => new Limits(cpuSeconds: null, wallClockSeconds: 1.0),
	'both' => new Limits(cpuSeconds: 1.0, wallClockSeconds: 1.0),
] as $label => $limits) {
	try {
		$sandbox->setLimits($limits);
		printf("setLimits %s => ACCEPTED\n", $label);
	} catch (ConfigurationError) {
		printf("setLimits %s => ConfigurationError\n", $label);
	}
}

// Refused means unchanged, not half-applied: the sandbox still reports no
// timing limits, and still works.
$readBack = $sandbox->limits();

printf("cpu after  => %s\n", var_export($readBack->cpuSeconds, true));
printf("wall after => %s\n", var_export($readBack->wallClockSeconds, true));

// A limit that does NOT conflict still applies through the same setter.
$sandbox->setLimits(new Limits(cpuSeconds: null, wallClockSeconds: null, memoryBytes: 4 * 1024 * 1024));
printf("memory     => %d\n", $sandbox->limits()->memoryBytes);

[$alive] = $sandbox->eval('return 1 + 1', '=alive');
printf("still runs => %d\n", $alive);

$sandbox->close();

?>
--EXPECT--
construct  => ConfigurationError
setLimits cpu  => ConfigurationError
setLimits wall => ConfigurationError
setLimits both => ConfigurationError
cpu after  => NULL
wall after => NULL
memory     => 4194304
still runs => 2
