--TEST--
Sandbox::features() reports what this platform can actually enforce
--EXTENSIONS--
luaext
--FILE--
<?php

use DevelopGravity\LuaExt\LimitSupport;
use DevelopGravity\LuaExt\Sandbox;

$features = Sandbox::features();

var_dump(array_keys($features));

var_dump($features['cpuLimit'] instanceof LimitSupport);
var_dump($features['wallClockLimit'] instanceof LimitSupport);
var_dump(is_float($features['cpuResolutionSeconds']));
var_dump($features['threadSafe'] === (PHP_ZTS === 1));
var_dump(is_string($features['platform']) && $features['platform'] !== '');

// The watchdog does not exist yet, so neither limit is enforced and this says
// so rather than pretending. Update this assertion when the watchdog lands --
// never the value it checks.
var_dump($features['cpuLimit'] === LimitSupport::Unsupported);
var_dump($features['wallClockLimit'] === LimitSupport::Unsupported);

?>
--EXPECT--
array(5) {
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
}
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
