--TEST--
No coroutine construct can swallow a CPU-limit breach
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
 * coroutine.resume is a protected call wearing a different hat: it returns
 * false plus the error rather than propagating it, which is exactly the shape
 * of every other construct that can swallow a limit breach. coroutine.wrap
 * re-raises, so it looks safer -- until a pcall is put around it.
 *
 * Both need the sandbox's own coroutine wrapper, which caps live coroutines and
 * refuses to let resume swallow a fatal. Upstream's luaopen_coroutine does
 * neither, which is why LUAEXT_LIB_CORO stays clear even for a trusted sandbox
 * and why these two cannot pass until the wrapper lands.
 *
 * They are here rather than in fatal-error-uncatchable.phpt because that test
 * is the one that says "no Lua construct can swallow this" for the constructs
 * that exist today, and it should be green.
 */

const CPU_SECONDS = 0.05;
const WALL_BACKSTOP_SECONDS = 0.5;

$attacks = [
	'coroutine.resume' => 'local c = coroutine.create(function() while true do end end)
		coroutine.resume(c) return "swallowed"',
	'coroutine.wrap' => 'local w = coroutine.wrap(function() while true do end end)
		pcall(w) return "swallowed"',

	// A resume nested inside a pcall inside another coroutine: three protected
	// calls, of two different kinds, between the loop and the boundary.
	'nested resume' => 'local inner = coroutine.create(function() while true do end end)
		local outer = coroutine.create(function() pcall(coroutine.resume, inner) end)
		coroutine.resume(outer) return "swallowed"',
];

foreach ($attacks as $label => $code) {
	// A fresh sandbox each time: a spent budget would stop the next attack
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
coroutine.resume     stopped
coroutine.wrap       stopped
nested resume        stopped
