--TEST--
Sandbox::features() reports what this build can enforce, not what it would like to
--EXTENSIONS--
luaext
--INI--
luaext.hook_count=1000
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\LimitSupport;
use DevelopGravity\LuaExt\Sandbox;

/*
 * This method exists so that a host which must not run untrusted code without a
 * time limit can find out that it has none. The one thing it may never do is
 * report a limit it does not enforce, so every case here is about honesty
 * rather than about capability.
 */

$features = Sandbox::features();

// Enforced or Degraded, never Unsupported: this build has the count hook and
// every platform CI runs on has a per-thread CPU clock. Which of the two it is
// depends on how coarse that clock is, and asserting the specific case here
// would turn this into a test of the runner.
var_dump($features['cpuLimit'] !== LimitSupport::Unsupported);

// A resolution of zero means "no clock at all", which would contradict the
// line above. Anything coarser than a second is not a clock either.
var_dump($features['cpuResolutionSeconds'] > 0.0);
var_dump($features['cpuResolutionSeconds'] < 1.0);

// Degraded is reserved for a coarse clock -- Windows resolves thread CPU time
// on the ~15.6 ms scheduler tick. Anything finer than a millisecond can be
// enforced, and this derives the expectation from the reported resolution
// rather than from the platform name, so one assertion covers all three.
$fine = $features['cpuResolutionSeconds'] <= 0.001;
var_dump($features['cpuLimit'] === ($fine ? LimitSupport::Enforced : LimitSupport::Degraded));

// The wall-clock limit needs the watchdog thread to cover a script that is
// blocked OUTSIDE the interpreter. It degrades rather than disappears when the
// thread cannot be created, because the count hook still delivers it the moment
// Lua executes another instruction.
var_dump($features['wallClockLimit'] !== LimitSupport::Unsupported);

// Nothing above is per-sandbox. features() is static, has no configured limit
// to judge, and must answer identically however many sandboxes exist -- the
// "this particular limit is too fine for this clock" decision belongs to
// setLimits(), which knows the limit.
$sandbox = new Sandbox();
var_dump(Sandbox::features() == $features);
$sandbox->close();
var_dump(Sandbox::features() == $features);

?>
--EXPECT--
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
