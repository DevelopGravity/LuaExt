--TEST--
The profiler samples coroutines that already existed when it was enabled, and disarms them all
--EXTENSIONS--
luaext
--SKIPIF--
<?php
// Same skip as sampling-attributes-cost.phpt: on a build whose watchdog thread
// could not start, the count hook IS the CPU limit and enableProfiler()
// refuses rather than removing it -- no samples can exist to assert on.
use DevelopGravity\LuaExt\LimitSupport;
use DevelopGravity\LuaExt\Sandbox;

if (Sandbox::features()['cpuLimit'] === LimitSupport::Unsupported) {
	echo "skip this build reports LimitSupport::Unsupported for the CPU limit";
}
?>
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\ProfilerUnit;
use DevelopGravity\LuaExt\Sandbox;

// lua_sethook is per-thread, and a coroutine only inherits the hook its
// creator carried at lua_newthread time. So a coroutine created BEFORE
// enableProfiler() is the case that goes unsampled if the profiler arms only
// the main state -- and since the call-boundary sweep closes every suspended
// coroutine, the one way such a coroutine exists is a host callback flipping
// the profiler on mid-call, from inside the very call that created it.
$sandbox = new Sandbox();

$sandbox->registerLibrary('host', [
	'enable' => static function () use ($sandbox): bool {
		return $sandbox->enableProfiler(0.000001);
	},
	'disable' => static function () use ($sandbox): void {
		$sandbox->disableProfiler();
	},
]);

[$enabled, $burned] = $sandbox->eval('
	local co = coroutine.create(function ()
		coroutine.yield()

		-- The hot function, so its samples are attributable to this line.
		local function inside_coroutine()
			local total = 0
			for index = 1, 2000000 do
				total = total + index % 7
			end
			return total
		end

		return inside_coroutine()
	end)

	-- The coroutine exists and is suspended before the profiler is switched
	-- on; everything hot then runs inside it.
	coroutine.resume(co)
	local enabled = host.enable()
	local ok, total = coroutine.resume(co)
	host.disable()

	return enabled, total
', '=sampled');

var_dump($enabled, $burned > 0);

// The samples must exist, and the hot function must be attributed to the
// coroutine's chunk -- not lost because the thread never carried the hook.
$profile = $sandbox->getProfile(ProfilerUnit::Samples);

$sampledInside = 0.0;

foreach ($profile as $identity => $samples) {
	if (str_contains((string) $identity, 'sampled')) {
		$sampledInside += $samples;
	}
}

var_dump(array_sum($profile) > 0, $sampledInside > 0);

$sandbox->close();

?>
--EXPECT--
bool(true)
bool(true)
bool(true)
bool(true)
