--TEST--
Sandbox::features() reports what this platform can actually enforce
--EXTENSIONS--
luaext
--FILE--
<?php

use DevelopGravity\LuaExt\Capabilities;
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

/*
 * The capabilities map has to cover exactly the boolean flags a Capabilities
 * object carries, and that correspondence is asserted rather than eyeballed:
 * the map is a hand-maintained literal in C, so the failure it invites is
 * someone adding a capability and forgetting it here. A missing key reads as
 * "this build does not implement it" to any host that checks, which is the
 * under-reporting direction and the one worth failing the build over.
 *
 * osEnvAllowList is excluded because it is a list of names, not a toggle.
 */
$declared = array_map(
	static fn (ReflectionProperty $property): string => $property->getName(),
	(new ReflectionClass(Capabilities::class))->getProperties(),
);
$toggles = array_values(array_diff($declared, ['osEnvAllowList']));

sort($toggles);
$reported = array_keys($features['capabilities']);
sort($reported);

var_dump($toggles === $reported);
var_dump(array_reduce($features['capabilities'], static fn ($carry, $v) => $carry && is_bool($v), true));

// Named individually so that implementing one is a visible, deliberate edit
// here rather than a silent flip. See docs/lua-api.md for what each still lacks.
var_dump(array_keys(array_filter($features['capabilities'], static fn (bool $v): bool => !$v)));

?>
--EXPECT--
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
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
array(4) {
  [0]=>
  string(7) "require"
  [1]=>
  string(3) "vfs"
  [2]=>
  string(8) "vfsWrite"
  [3]=>
  string(10) "coroutines"
}
