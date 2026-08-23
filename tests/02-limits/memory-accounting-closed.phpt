--TEST--
The memory methods refuse a closed sandbox rather than reading freed counters
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Exception\ClosedSandboxError;
use DevelopGravity\LuaExt\Sandbox;

$sandbox = new Sandbox();
$sandbox->close();

// close() has already run lua_close(), so the counters describe a heap that no
// longer exists. Reporting a stale number would be worse than refusing.
$calls = [
	'getMemoryUsage' => static fn (Sandbox $sandbox): int => $sandbox->getMemoryUsage(),
	'getPeakMemoryUsage' => static fn (Sandbox $sandbox): int => $sandbox->getPeakMemoryUsage(),
	'setMemoryLimit' => static function (Sandbox $sandbox): void {
		$sandbox->setMemoryLimit(1024);
	},
];

foreach ($calls as $name => $call) {
	try {
		$call($sandbox);
	} catch (ClosedSandboxError $error) {
		printf("%s: %s: %s\n", $name, $error::class, $error->getMessage());
	}
}

// The closed check comes before argument validation, so a caller learns the
// real problem rather than being told about an argument to a method that was
// never going to run.
try {
	$sandbox->setMemoryLimit(0);
} catch (ClosedSandboxError $error) {
	printf("invalid argument on closed sandbox: %s\n", $error::class);
}

?>
--EXPECT--
getMemoryUsage: DevelopGravity\LuaExt\Exception\ClosedSandboxError: The sandbox has been closed
getPeakMemoryUsage: DevelopGravity\LuaExt\Exception\ClosedSandboxError: The sandbox has been closed
setMemoryLimit: DevelopGravity\LuaExt\Exception\ClosedSandboxError: The sandbox has been closed
invalid argument on closed sandbox: DevelopGravity\LuaExt\Exception\ClosedSandboxError
