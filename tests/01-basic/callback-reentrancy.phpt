--TEST--
A callback can call back into Lua, and a failure inside one still escapes every frame
--EXTENSIONS--
luaext
--XFAIL--
Needs the pcall replacement from the library-policy wave. The bridge, its boundary accounting and the wiring are all in place, and the fatal is raised correctly -- but Lua's own pcall is still upstream's, so a script can catch a host failure on the way in. Everything above the last assertion already passes.
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\LuaFunction;
use DevelopGravity\LuaExt\Sandbox;

$sandbox = new Sandbox();

// A Lua function argument arrives as a LuaFunction, so a callback can call
// straight back into the script that called it. Every crossing has to keep the
// boundary accounting straight, or a later wave cannot tell whose CPU time it
// is billing.
$sandbox->registerLibrary('host', [
	'apply' => static fn (LuaFunction $callback, mixed $value): mixed => $callback($value)[0],
]);

// Lua -> PHP -> Lua -> PHP -> Lua, and back out with the right answer.
var_dump($sandbox->eval(<<<'LUA'
	local function double(value) return value * 2 end

	return host.apply(function(value) return host.apply(double, value) + 1 end, 20)
LUA));

// Two crossings, however deeply the second was nested inside the first.
var_dump($sandbox->stats()->phpCallsOut);

$sandbox->registerLibrary('deep', [
	'call' => static fn (LuaFunction $callback): mixed => $callback()[0],
	'fail' => static function (): never { throw new LogicException('from the inside'); },
]);

// The failure happens in the innermost PHP frame, crosses back through a Lua
// frame and another PHP frame, and is still not something the outer pcall may
// swallow on the way past.
try {
	(void) $sandbox->eval(<<<'LUA'
		local ok = pcall(function()
			return deep.call(function() return deep.fail() end)
		end)

		return "swallowed"
LUA);
	echo "SWALLOWED\n";
} catch (LogicException $error) {
	printf("escaped every frame: %s\n", $error->getMessage());
}

$sandbox->close();

?>
--EXPECT--
array(1) {
  [0]=>
  int(41)
}
int(2)
escaped every frame: from the inside
