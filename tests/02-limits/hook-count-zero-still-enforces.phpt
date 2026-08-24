--TEST--
luaext.hook_count=0 no longer weakens a limit, because the interpreter carries the check itself
--EXTENSIONS--
luaext
--INI--
luaext.hook_count=0
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Exception\CpuLimitError;
use DevelopGravity\LuaExt\LimitSupport;
use DevelopGravity\LuaExt\Limits;
use DevelopGravity\LuaExt\Sandbox;
use DevelopGravity\LuaExt\SandboxConfig;

/*
 * This INI used to void both limits, and this test used to assert that it did.
 *
 * It no longer does. The count hook is not the delivery mechanism any more: the
 * interpreter checks the interrupt flag at its own back edges -- backward jump,
 * both 'for' loops, tail call -- so a runaway script is stopped whether or not a
 * hook is armed. The hook survives only as the fallback that reads the clock
 * when the watchdog thread could not be started.
 *
 * What that leaves this INI meaning: no self-check. The flag still has to be SET
 * by somebody, and normally that is the watchdog thread. Only if the thread also
 * fails is there nothing left to notice a breach -- and features() says so then,
 * which is what the honesty contract requires. Here the thread starts, so
 * everything is enforced.
 */

const CPU_SECONDS = 0.05;

// The feature report is not degraded: nothing about enforcement changed.
$features = Sandbox::features();
var_dump($features['cpuLimit'] !== LimitSupport::Unsupported);
var_dump($features['wallClockLimit'] !== LimitSupport::Unsupported);

// The default sandbox asks for a one-second CPU limit and builds fine.
$sandbox = new Sandbox();
var_dump($sandbox->eval('return 6 * 7')[0]);
$sandbox->close();

// The setters accept a limit rather than refusing it.
$sandbox = new Sandbox(new SandboxConfig(
	limits: (new Limits())->with(cpuSeconds: null, wallClockSeconds: null),
));
$sandbox->setCpuLimit(1.0);
$sandbox->setWallClockLimit(2.0);
echo "both setters accepted\n";
$sandbox->close();

/*
 * And the part that actually matters: with no hook anywhere in the interpreter,
 * every shape of unbounded loop is still stopped. Each of these reaches the flag
 * through a different back edge, which is why they are listed separately rather
 * than as one representative case.
 */
$shapes = [
	'while true' => 'while true do end',
	'repeat until false' => 'repeat until false',
	'numeric for' => 'for i = 1, math.maxinteger do end',
	'tail recursion' => 'local function f() return f() end f()',
	'backward goto' => '::top:: goto top',
];

foreach ($shapes as $label => $chunk) {
	$bounded = new Sandbox(new SandboxConfig(
		limits: (new Limits())->with(cpuSeconds: CPU_SECONDS, wallClockSeconds: 3.0),
	));

	try {
		(void) $bounded->eval($chunk, '=unbounded');
		printf("%-20s ESCAPED\n", $label);
	} catch (CpuLimitError) {
		printf("%-20s stopped\n", $label);
	} catch (Throwable $error) {
		printf("%-20s WRONG CLASS: %s\n", $label, $error::class);
	}

	$bounded->close();
}

?>
--EXPECT--
bool(true)
bool(true)
int(42)
both setters accepted
while true           stopped
repeat until false   stopped
numeric for          stopped
tail recursion       stopped
backward goto        stopped
