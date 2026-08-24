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
// PHP_ZTS is a bool, so comparing it against 1 is always false and would make
// this assert "the extension is never thread safe" on every platform.
var_dump($features['threadSafe'] === PHP_ZTS);
var_dump(is_string($features['platform']) && $features['platform'] !== '');

// The watchdog exists, so neither limit may report Unsupported on a platform
// that has a per-thread CPU clock and the default luaext.hook_count. Which of
// Enforced and Degraded it is depends on how coarse that clock is -- ~1 ns on
// Linux, ~1 us on macOS, ~15.6 ms on Windows -- so asserting the exact case
// here would make this test a platform test. tests/02-limits/features-honesty
// covers the specific cases, including the one where an INI voids them.
var_dump($features['cpuLimit'] !== LimitSupport::Unsupported);
var_dump($features['wallClockLimit'] !== LimitSupport::Unsupported);

// A resolution of zero would mean "no clock", which contradicts the two above.
var_dump($features['cpuResolutionSeconds'] > 0.0);

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
bool(true)
