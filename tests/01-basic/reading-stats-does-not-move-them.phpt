--TEST--
stats() is an observer: two successive reads report the same memory
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Capabilities;
use DevelopGravity\LuaExt\Sandbox;
use DevelopGravity\LuaExt\SandboxConfig;

// The live-coroutine recount used to CREATE its tracking table when it was
// absent -- on a sandbox never granted coroutines, and on every sandbox after
// the end-of-call sweep detaches it -- so reading the memory stats grew the
// very heap they report. An observer must not move what it observes.

$assertStable = static function (string $label, Sandbox $sandbox): void {
	$first = $sandbox->stats()->memoryBytes;
	$second = $sandbox->stats()->memoryBytes;

	printf(
		"%-24s %s (%+d bytes between reads)\n",
		$label,
		$first === $second ? 'stable' : 'MOVED',
		$second - $first,
	);
};

// The capability-off shape: the tracking table has never existed.
$without = new Sandbox(new SandboxConfig(
	capabilities: (new Capabilities())->with(coroutines: false),
));
(void) $without->eval('return 1', '=warm');
$assertStable('coroutines off', $without);
$without->close();

// The post-sweep shape: a coroutine ran, the end-of-call sweep detached the
// table, and nothing but a real create() may bring it back.
$with = new Sandbox(new SandboxConfig());
(void) $with->eval(<<<'LUA'
	local co = coroutine.create(function() return 1 end)
	coroutine.resume(co)
	return 1
LUA, '=co');
$assertStable('after a swept coroutine', $with);

// The non-creating lookup does not under-report either: a coroutine left
// suspended was closed by the sweep, and the recount agrees with the counter
// the sweep zeroed.
(void) $with->eval('suspended = coroutine.create(function() coroutine.yield() end) coroutine.resume(suspended)', '=live');
printf("%-24s %d\n", 'live after the sweep', $with->stats()->liveCoroutines);

$with->close();

?>
--EXPECT--
coroutines off           stable (+0 bytes between reads)
after a swept coroutine  stable (+0 bytes between reads)
live after the sweep     0
