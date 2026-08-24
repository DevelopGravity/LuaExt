--TEST--
luaext.hook_count=0 removes the only mechanism that can stop a script, and says so
--EXTENSIONS--
luaext
--INI--
luaext.hook_count=0
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Exception\ConfigurationError;
use DevelopGravity\LuaExt\LimitSupport;
use DevelopGravity\LuaExt\Limits;
use DevelopGravity\LuaExt\Sandbox;
use DevelopGravity\LuaExt\SandboxConfig;

/*
 * The count hook is the only thing that can interrupt lvm.c's dispatch loop.
 * The watchdog thread does not stop scripts; it raises a flag, and the hook is
 * what turns that flag into a stopped script. Remove the hook and BOTH limits
 * become suggestions -- including the wall-clock one, whose name does not hint
 * at the dependency.
 *
 * An INI that silently voids a security guarantee is exactly what this
 * extension exists to prevent, so setting this one is loud in three places: the
 * feature report, the limit setters, and construction itself.
 */

// One: the feature report. Not Degraded -- there is no degraded mode left when
// the delivery mechanism is gone.
$features = Sandbox::features();
var_dump($features['cpuLimit'] === LimitSupport::Unsupported);
var_dump($features['wallClockLimit'] === LimitSupport::Unsupported);

// Two: construction. The default SandboxConfig asks for a one-second CPU limit,
// so the default sandbox cannot be built at all. Refusing here is the whole
// point: the alternative is a sandbox that looks bounded and is not.
try {
	$sandbox = new Sandbox();
	echo "CONSTRUCTED ANYWAY\n";
} catch (ConfigurationError $error) {
	echo "refused: ", $error->getMessage(), "\n";
}

// Three: the setters, for a sandbox that was built without a limit and is later
// handed one.
$unbounded = new Sandbox(new SandboxConfig(
	limits: (new Limits())->with(cpuSeconds: null, wallClockSeconds: null),
));

try {
	$unbounded->setCpuLimit(1.0);
	echo "ACCEPTED ANYWAY\n";
} catch (ConfigurationError) {
	echo "setCpuLimit refused\n";
}

try {
	$unbounded->setWallClockLimit(1.0);
	echo "ACCEPTED ANYWAY\n";
} catch (ConfigurationError) {
	echo "setWallClockLimit refused\n";
}

// Lifting a limit is still allowed: null asks for nothing and gets it.
$unbounded->setCpuLimit(null);
$unbounded->setWallClockLimit(null);
echo "lifting a limit is still allowed\n";

// And a sandbox that never asked for a time limit still works. This INI is a
// deliberate choice for a host that bounds its scripts some other way, not a
// broken build.
var_dump($unbounded->eval('return 6 * 7', '=arithmetic'));

$unbounded->close();

?>
--EXPECTF--
bool(true)
bool(true)
refused: DevelopGravity\LuaExt\Sandbox::setCpuLimit() cannot be honoured: luaext.hook_count is 0, %s
setCpuLimit refused
setWallClockLimit refused
lifting a limit is still allowed
array(1) {
  [0]=>
  int(42)
}
