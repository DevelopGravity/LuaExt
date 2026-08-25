--TEST--
A Sandbox accepts a configured SandboxConfig and holds it for the sandbox's life
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Capabilities;
use DevelopGravity\LuaExt\Limits;
use DevelopGravity\LuaExt\LimitSupport;
use DevelopGravity\LuaExt\OutputMode;
use DevelopGravity\LuaExt\Sandbox;
use DevelopGravity\LuaExt\SandboxConfig;

$config = new SandboxConfig(
	capabilities: (new Capabilities())->with(utf8: false, coroutines: false),
	limits: (new Limits())->with(memoryBytes: 4 * 1024 * 1024, cpuSeconds: 0.25),
	outputMode: OutputMode::Discard,
	seed: 99,
	deterministic: true,
);

$sandbox = new Sandbox($config);
var_dump($sandbox->isClosed());

/*
 * A host that must not run untrusted code without a time limit can find out
 * whether it has one. Asserted as "not Unsupported" rather than as a specific
 * case, because which of Enforced and Degraded applies is a property of the
 * platform clock and this test is about the config object.
 */
$features = Sandbox::features();
var_dump($features['cpuLimit'] !== LimitSupport::Unsupported);
var_dump($features['wallClockLimit'] !== LimitSupport::Unsupported);
var_dump(array_keys($features));

// The config object survives being handed over: the sandbox holds a reference
// so the filesystem, resolver and output callback outlive the constructor call.
var_dump($config->seed, $config->capabilities?->utf8);

$sandbox->close();
var_dump($sandbox->isClosed());

// The same config builds a second sandbox: nothing was consumed or mutated.
$second = new Sandbox($config);
var_dump($second->isClosed());
$second->close();

echo "done\n";

?>
--EXPECT--
bool(false)
bool(true)
bool(true)
array(6) {
  [0]=>
  string(8) "cpuLimit"
  [1]=>
  string(14) "wallClockLimit"
  [2]=>
  string(20) "cpuResolutionSeconds"
  [3]=>
  string(10) "threadSafe"
  [4]=>
  string(8) "platform"
  [5]=>
  string(12) "capabilities"
}
int(99)
bool(false)
bool(true)
bool(false)
done
