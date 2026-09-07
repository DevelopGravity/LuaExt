--TEST--
stats()->peakCoroutineDepth records the deepest nesting and never runs backwards
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Exception\CoroutineLimitError;
use DevelopGravity\LuaExt\Limits;
use DevelopGravity\LuaExt\Sandbox;
use DevelopGravity\LuaExt\SandboxConfig;

// Until now the only coverage this field had was a reflection walk over a
// never-populated object, where every value is the PHP zero -- a test that
// would keep passing if the counter were never wired up at all. This binds
// the number to behavior: it climbs with real nesting, it is a high-water
// mark rather than a per-call reading, and it cannot exceed the depth cap.

$nest = <<<'LUA'
	local function nest(n)
		if n == 0 then
			return 0
		end
		local co = coroutine.create(function()
			return nest(n - 1)
		end)
		local ok, value = coroutine.resume(co)
		return value + 1
	end
	return nest(%d)
LUA;

$sandbox = new Sandbox(new SandboxConfig());

printf("fresh:        %d\n", $sandbox->stats()->peakCoroutineDepth);

(void) $sandbox->eval(sprintf($nest, 3), '=deep');
printf("after depth 3: %d\n", $sandbox->stats()->peakCoroutineDepth);

// A shallower call cannot lower a high-water mark.
(void) $sandbox->eval(sprintf($nest, 1), '=shallow');
printf("after depth 1: %d\n", $sandbox->stats()->peakCoroutineDepth);

$sandbox->close();

// Under a cap the mark can reach the cap and no further: the resume that
// would nest past it is refused before it deepens anything.
$capped = new Sandbox(new SandboxConfig(
	limits: (new Limits())->with(maxCoroutineDepth: 2),
));

try {
	(void) $capped->eval(sprintf($nest, 3), '=capped');
	print "capped:       RAN PAST THE CAP\n";
} catch (CoroutineLimitError $error) {
	printf("capped at:    %d\n", $capped->stats()->peakCoroutineDepth);
}

$capped->close();

?>
--EXPECT--
fresh:        0
after depth 3: 3
after depth 1: 3
capped at:    2
