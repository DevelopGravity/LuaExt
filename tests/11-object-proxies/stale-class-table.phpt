--TEST--
A class table aliased across unregister() refuses instead of reaching a freed record
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\LuaMethod;
use DevelopGravity\LuaExt\Sandbox;

final class Clock
{
	#[LuaMethod]
	public static function now(): int { return 42; }
}

final class Money
{
	#[LuaMethod]
	public function __construct(public readonly int $amount = 0) {}

	#[LuaMethod]
	public static function zero(): int { return 0; }
}

$sandbox = new Sandbox();

// Statics-only class: live_proxies is zero, so unregister() reclaims the
// record immediately. The aliased table must refuse, not dispatch through it.
$sandbox->registerClass(Clock::class);
(void) $sandbox->eval('M = Clock');
$sandbox->unregister('Clock');
var_dump($sandbox->eval('local ok, err = pcall(function() return M.now() end) return ok, tostring(err)'));

// Constructor and statics: both the table alias and a direct closure alias
// (`Money.new` grabbed before the unregister) must refuse.
$sandbox->registerClass(Money::class);
(void) $sandbox->eval('T = Money; f = Money.new; z = Money.zero');
$sandbox->unregister('Money');
var_dump($sandbox->eval('local ok, err = pcall(function() return T.new(5) end) return ok, tostring(err)'));
var_dump($sandbox->eval('local ok, err = pcall(function() return f(5) end) return ok, tostring(err)'));
var_dump($sandbox->eval('local ok, err = pcall(function() return z() end) return ok, tostring(err)'));

// The script can drive the whole sequence itself through a host callback that
// retires the class mid-call; the closure it saved a moment earlier refuses.
$sandbox->registerClass(Money::class);
$sandbox->registerLibrary('host', ['retire' => static function () use ($sandbox): bool {
	$sandbox->unregister('Money');
	return true;
}]);
var_dump($sandbox->eval(<<<'LUA'
	local g = Money.new
	host.retire()
	local ok, err = pcall(g, 5)
	return ok, tostring(err)
LUA));

// A fresh registration under the same name plants a fresh table; the stale
// alias stays revoked while the new one dispatches.
$sandbox->registerClass(Clock::class);
var_dump($sandbox->eval('return Clock.now()')[0]);
var_dump($sandbox->eval('local ok = pcall(function() return M.now() end) return ok')[0]);

$sandbox->close();

?>
--EXPECT--
array(2) {
  [0]=>
  bool(false)
  [1]=>
  string(48) "'now' cannot run: its registration was withdrawn"
}
array(2) {
  [0]=>
  bool(false)
  [1]=>
  string(52) "Money.new cannot run: its registration was withdrawn"
}
array(2) {
  [0]=>
  bool(false)
  [1]=>
  string(52) "Money.new cannot run: its registration was withdrawn"
}
array(2) {
  [0]=>
  bool(false)
  [1]=>
  string(49) "'zero' cannot run: its registration was withdrawn"
}
array(2) {
  [0]=>
  bool(false)
  [1]=>
  string(52) "Money.new cannot run: its registration was withdrawn"
}
int(42)
bool(false)
