--TEST--
Planting and clearing class globals is raw: a script's _G metamethods never run
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

$sandbox = new Sandbox();

// A script installs a __newindex trap on the globals table. Host-side
// registration writes must bypass it — otherwise arbitrary script code runs
// unmetered inside registerClass()/unregister().
(void) $sandbox->eval(<<<'LUA'
	hooked = 0
	setmetatable(_G, {
		__newindex = function(table, key, value)
			hooked = hooked + 1
			rawset(table, key, value)
		end,
	})
LUA);

$sandbox->registerClass(Clock::class);
var_dump($sandbox->eval('return rawget(_G, "Clock") ~= nil, hooked'));

$sandbox->unregister('Clock');
var_dump($sandbox->eval('return rawget(_G, "Clock") == nil, hooked'));

// Script writes still honour the trap, so the raw path is host-only.
(void) $sandbox->eval('fresh_global = true');
var_dump($sandbox->eval('return hooked')[0]);

$sandbox->close();

?>
--EXPECT--
array(2) {
  [0]=>
  bool(true)
  [1]=>
  int(0)
}
array(2) {
  [0]=>
  bool(true)
  [1]=>
  int(0)
}
int(1)
