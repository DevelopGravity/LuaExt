--TEST--
No Lua construct can swallow a CPU-limit breach
--EXTENSIONS--
luaext
--SKIPIF--
<?php
// The only --SKIPIF-- shape this suite allows, and it earns its place: every
// assertion below runs an unbounded loop to prove the CPU limit stops it. On a
// build where features() says the limit cannot be enforced at all, running an
// infinite loop to demonstrate that it is not enforced is pure waste -- and the
// harness would have to time each one out. The build that reports Unsupported
// is covered by tests/02-limits/hook-count-zero-voids-limits.phpt instead.
use DevelopGravity\LuaExt\LimitSupport;
use DevelopGravity\LuaExt\Sandbox;

if (Sandbox::features()['cpuLimit'] === LimitSupport::Unsupported) {
	echo "skip this build reports LimitSupport::Unsupported for the CPU limit";
}
?>
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Exception\CpuLimitError;
use DevelopGravity\LuaExt\Limits;
use DevelopGravity\LuaExt\Sandbox;
use DevelopGravity\LuaExt\SandboxConfig;

/*
 * The whole extension rests on this. If any of these constructs can catch a
 * limit breach and carry on, then setCpuLimit() is a suggestion and untrusted
 * code runs for as long as it likes. Every one of them is a protected call in
 * disguise, and every one of them has to re-raise instead of returning.
 *
 * Two mechanisms make that hold, and both are needed. The sandbox's pcall
 * replacement refuses to catch the unforgeable fatal userdata, which stops the
 * script at the pcall with the right traceback. Behind it, the interrupt flag
 * stays raised until the outermost call has unwound, so anything that still
 * gets past -- a place Lua swallows errors that cannot be patched away -- is
 * reported at the boundary rather than returned as a result.
 *
 * The two coroutine attacks live in coroutine-cannot-swallow-fatal.phpt: the
 * coroutine library is deliberately not installed yet, so they fail on a nil
 * index rather than on the limit, and keeping them here would have made this
 * whole file XFAIL for a reason that has nothing to do with these five.
 *
 * The wall-clock limit is an order of magnitude above the CPU limit, purely so
 * a regression fails in half a second instead of hanging CI.
 */

const CPU_SECONDS = 0.05;
const WALL_BACKSTOP_SECONDS = 0.5;

$attacks = [
	'pcall' => 'pcall(function() while true do end end) return "swallowed"',
	'nested pcall' => 'pcall(function() pcall(function() while true do end end) end) return "swallowed"',
	'xpcall' => 'xpcall(function() while true do end end, function() return "handled" end) return "swallowed"',
	'in a finaliser' => 'local t = setmetatable({}, {__gc = function() while true do end end})
		t = nil collectgarbage("collect") return "swallowed"',
	'in a to-be-closed' => 'do local _ <close> = setmetatable({}, {__close = function() while true do end end}) end
		return "swallowed"',
];

foreach ($attacks as $label => $code) {
	// A fresh sandbox each time: a spent CPU budget would trip the next attack
	// before it ever ran, which would prove nothing.
	$sandbox = new Sandbox(new SandboxConfig(
		limits: (new Limits())->with(
			cpuSeconds: CPU_SECONDS,
			wallClockSeconds: WALL_BACKSTOP_SECONDS,
		),
	));

	try {
		$result = $sandbox->eval($code, '=attack');
		printf("%-20s LIMIT ESCAPED: %s\n", $label, var_export($result, true));
	} catch (CpuLimitError) {
		printf("%-20s stopped\n", $label);
	} catch (Throwable $error) {
		printf("%-20s WRONG CLASS: %s\n", $label, $error::class);
	}

	$sandbox->close();
}

?>
--EXPECT--
pcall                stopped
nested pcall         stopped
xpcall               stopped
in a finaliser       stopped
in a to-be-closed    stopped
