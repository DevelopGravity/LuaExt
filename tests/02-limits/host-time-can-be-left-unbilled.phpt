--TEST--
Limits::$billHostTime = false stops host crossings from spending the timing budgets
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Capabilities;
use DevelopGravity\LuaExt\Exception\WallClockLimitError;
use DevelopGravity\LuaExt\Limits;
use DevelopGravity\LuaExt\ModuleResolver;
use DevelopGravity\LuaExt\ModuleSource;
use DevelopGravity\LuaExt\OutputMode;
use DevelopGravity\LuaExt\Sandbox;
use DevelopGravity\LuaExt\SandboxConfig;

// Host code is unbilled by default: every crossing -- a registered callable,
// the output callback, a ModuleResolver -- pauses both clocks for exactly its
// own duration, under the rules pauseTimers() already established. Lua
// re-entered from a callback is still billed, and a callback that calls
// resumeTimers() opts back in. Limits::$billHostTime = true restores the old
// behaviour, where host time is the script's time.
//
// Each callable here blocks for several times the wall budget. Billed, the
// breach lands the moment the callback returns; unbilled -- the default --
// the call completes and the stats never saw the time.

const WALL_LIMIT = 0.25;
const BLOCK_SECONDS = 0.4;

$block = static function (): int {
	usleep((int) (BLOCK_SECONDS * 1_000_000));

	return 1;
};

$resolver = new class implements ModuleResolver {
	public function resolve(string $name, string $requestedBy): ?ModuleSource
	{
		usleep((int) (0.4 * 1_000_000));

		return new ModuleSource('return 7', '=slowmod');
	}
};

$build = static function (?bool $billed) use ($block, $resolver): Sandbox {
	// Null exercises the DEFAULT rather than restating it: the unbilled probes
	// must pass with billHostTime never mentioned at all.
	$limits = $billed === null
		? new Limits(cpuSeconds: 5.0, wallClockSeconds: WALL_LIMIT)
		: new Limits(cpuSeconds: 5.0, wallClockSeconds: WALL_LIMIT, billHostTime: $billed);

	$sandbox = new Sandbox(new SandboxConfig(
		capabilities: (new Capabilities())->with(require: true),
		limits: $limits,
		moduleResolver: $resolver,
		outputMode: OutputMode::Callback,
		outputCallback: static function (string $chunk, bool $isStderr) use ($block): void {
			$block();
		},
	));

	$sandbox->registerLibrary('host', ['block' => $block]);

	return $sandbox;
};

$attempt = static function (Sandbox $sandbox, string $script): string {
	try {
		(void) $sandbox->eval($script, '=probe');

		return sprintf('completed (wall %.2fs billed)', $sandbox->stats()->wallClockSeconds);
	} catch (WallClockLimitError) {
		return 'WallClockLimitError';
	}
};

// The opt-in control: with billing on, host time is the script's time and the
// blocked callable trips the wall limit the moment it returns.
$billed = $build(true);
printf("billed   callable: %s\n", $attempt($billed, 'return host.block()'));
$billed->close();

// The default, each crossing in turn. A fresh sandbox per probe so one
// crossing's clock cannot muddy the next reading.
foreach ([
	'callable' => 'return host.block()',
	'output  ' => 'print("through the callback") return 1',
	'resolver' => 'return require("slowmod")',
] as $label => $script) {
	$sandbox = $build(null);
	printf("default  %s: %s\n", $label, $attempt($sandbox, $script));
	$sandbox->close();
}

?>
--EXPECTF--
billed   callable: WallClockLimitError
default  callable: completed (wall 0.0%ds billed)
default  output  : completed (wall 0.0%ds billed)
default  resolver: completed (wall 0.0%ds billed)
