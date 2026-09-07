--TEST--
Limits::$maxLiveCoroutines refuses the population stats()->liveCoroutines reports
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Exception\CoroutineLimitError;
use DevelopGravity\LuaExt\Limits;
use DevelopGravity\LuaExt\Sandbox;
use DevelopGravity\LuaExt\SandboxConfig;

// The cap used to judge table MEMBERSHIP after a collection while the stat
// judged status: eight coroutines that finished but were still referenced
// counted as eight toward the cap and as zero in liveCoroutines, so the figure
// a host watches could not predict the refusal. Both must describe the same
// population -- threads that can still run.

$config = new SandboxConfig(limits: (new Limits())->with(maxLiveCoroutines: 8));

// Dead-but-referenced coroutines are not alive: the stat says zero, and the
// cap agrees by letting a ninth create() through.
$sandbox = new Sandbox($config);
$sandbox->registerLibrary('host', [
	'live' => static fn (): int => $sandbox->stats()->liveCoroutines,
]);

[$before, $after] = $sandbox->eval(<<<'LUA'
	local dead = {}
	for i = 1, 8 do
		dead[i] = coroutine.create(function() end)
		coroutine.resume(dead[i])
	end
	local before = host.live()
	local extra = coroutine.create(function() end)
	return before, host.live()
LUA, '=dead');
printf("dead referenced: stat %d, ninth create allowed, stat %d after it\n", $before, $after);
$sandbox->close();

// Suspended coroutines ARE alive: the ninth create refuses, fatally, at the
// moment the stat would have read eight.
$sandbox = new Sandbox($config);

try {
	(void) $sandbox->eval(<<<'LUA'
		local kept = {}
		for i = 1, 9 do
			kept[i] = coroutine.create(function() coroutine.yield() end)
			coroutine.resume(kept[i])
		end
	LUA, '=suspended');
	print "suspended: ninth create ALLOWED\n";
} catch (CoroutineLimitError $error) {
	printf("suspended: refused (%s)\n", $error->getMessage());
}

$sandbox->close();

?>
--EXPECT--
dead referenced: stat 0, ninth create allowed, stat 1 after it
suspended: refused (The sandbox already has 8 live coroutine(s), which is its Limits::$maxLiveCoroutines)
