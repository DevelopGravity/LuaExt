--TEST--
A parse fed by a reader observes the timing limits even with the source ceiling lifted
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Capabilities;
use DevelopGravity\LuaExt\Exception\WallClockLimitError;
use DevelopGravity\LuaExt\Limits;
use DevelopGravity\LuaExt\Sandbox;
use DevelopGravity\LuaExt\SandboxConfig;

// The parser consumes a reader function from a C loop the interrupt hooks
// cannot reach into, so the reader wrapper carries the check itself -- and it
// is installed whether or not a byte ceiling exists. With maxSourceBytes
// lifted, the wall clock is the only thing standing between an endless reader
// and a permanently wedged worker.

$sandbox = new Sandbox(new SandboxConfig(
	capabilities: (new Capabilities())->with(compileAtRuntime: true),
	limits: new Limits(
		cpuSeconds: 10.0,
		wallClockSeconds: 0.3,
		maxSourceBytes: 0,
	),
));

try {
	// Endless comments: syntactically fine forever, so only a limit ends it.
	(void) $sandbox->eval(<<<'LUA'
		load(function()
			return "-- more of the same\n"
		end)
		LUA, '=parse-forever');

	echo "parse => RAN\n";
} catch (WallClockLimitError) {
	echo "parse => WallClockLimitError\n";
}

$sandbox->close();

?>
--EXPECT--
parse => WallClockLimitError
