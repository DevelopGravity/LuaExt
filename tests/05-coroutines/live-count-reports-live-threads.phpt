--TEST--
stats()->liveCoroutines counts threads that are actually alive, not everything ever created
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Sandbox;

// The running counter behind this field is a high-water mark: it only
// re-syncs with the weak tracking table when maxLiveCoroutines is actually
// hit. Read mid-run from a host callback -- which the SandboxStats docblock
// explicitly invites -- it used to count every coroutine that had already
// finished and been collected. The stat now judges each tracked thread by
// its status, so a dead-but-uncollected thread does not count either.
$sandbox = new Sandbox();

$sandbox->registerLibrary('host', [
	'live' => static function () use ($sandbox): int {
		return $sandbox->stats()->liveCoroutines;
	},
]);

[$baseline, $midrun, $afterCollect] = $sandbox->eval('
	local baseline = host.live()

	-- Twelve coroutines run to completion: dead, and quite possibly not yet
	-- collected. One stays genuinely suspended.
	for index = 1, 12 do
		local co = coroutine.create(function () end)
		coroutine.resume(co)
	end

	local keep = coroutine.create(function () coroutine.yield() end)
	coroutine.resume(keep)

	local midrun = host.live()

	collectgarbage("collect")

	-- keep is still referenced, so the answer must not change with the
	-- garbage gone -- that is what proves status, not collection timing,
	-- decides the count.
	local after = host.live()

	-- keep survives to here, so the collector cannot have taken it early.
	coroutine.resume(keep)

	return baseline, midrun, after
', '=live');

var_dump($baseline, $midrun, $afterCollect);

// Back at the boundary the sweep has closed everything.
var_dump($sandbox->stats()->liveCoroutines);

$sandbox->close();

?>
--EXPECT--
int(0)
int(1)
int(1)
int(0)
