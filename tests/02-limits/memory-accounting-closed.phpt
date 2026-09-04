--TEST--
The accounting methods refuse a closed sandbox rather than reading freed counters
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Exception\ClosedSandboxError;
use DevelopGravity\LuaExt\Limits;
use DevelopGravity\LuaExt\Sandbox;

$sandbox = new Sandbox();
$sandbox->close();

// close() has already run lua_close(), so the counters describe a heap that no
// longer exists. Reporting a stale number would be worse than refusing.
$calls = [
	'stats' => static fn (Sandbox $sandbox): int => $sandbox->stats()->memoryBytes,
	'limits' => static fn (Sandbox $sandbox): ?int => $sandbox->limits()->memoryBytes,
	'setLimits' => static function (Sandbox $sandbox): void {
		$sandbox->setLimits(new Limits(memoryBytes: 1024));
	},
];

foreach ($calls as $name => $call) {
	try {
		$call($sandbox);
		printf("%s: NOT REFUSED\n", $name);
	} catch (ClosedSandboxError $error) {
		printf("%s: %s: %s\n", $name, $error::class, $error->getMessage());
	}
}

// The closed check comes before the argument is even read, so a caller learns
// the real problem rather than being told about a field of a Limits object that
// was never going to be applied. The Limits below is built independently for
// exactly that reason -- reading it off the sandbox would throw first and prove
// nothing about the ordering.
try {
	$sandbox->setLimits(new Limits(memoryBytes: -1));
	echo "invalid argument on closed sandbox: NOT REFUSED\n";
} catch (ClosedSandboxError $error) {
	printf("invalid argument on closed sandbox: %s\n", $error::class);
}

?>
--EXPECT--
stats: DevelopGravity\LuaExt\Exception\ClosedSandboxError: The sandbox has been closed
limits: DevelopGravity\LuaExt\Exception\ClosedSandboxError: The sandbox has been closed
setLimits: DevelopGravity\LuaExt\Exception\ClosedSandboxError: The sandbox has been closed
invalid argument on closed sandbox: DevelopGravity\LuaExt\Exception\ClosedSandboxError
