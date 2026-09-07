--TEST--
A logic exception that crossed the Lua boundary still serializes, dropping the sandbox
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Exception\ConfigurationError;
use DevelopGravity\LuaExt\Sandbox;
use DevelopGravity\LuaExt\SandboxConfig;

// LuaException redacts its sandbox in __serialize() because Sandbox wraps a
// live lua_State and cannot cross a process boundary. LuaLogicException used
// to skip that pair on the theory that host-misuse errors never touch Lua --
// but one thrown from inside a host callback crosses the boundary like any
// other host exception and picks up the sandbox, at which point serialize()
// on it threw. Both roots must redact the same way.

$sandbox = new Sandbox(new SandboxConfig());
$sandbox->registerLibrary('host', [
	'fail' => static function (): void {
		throw new ConfigurationError('deliberate host misuse', 7);
	},
]);

try {
	(void) $sandbox->eval('host.fail()', '=boom');
	print "eval returned\n";
} catch (ConfigurationError $error) {
	printf("carries the sandbox: %s\n", var_export($error->getSandbox() === $sandbox, true));

	$revived = unserialize(serialize($error));

	printf("revived:  %s: %s (code %d)\n", $revived::class, $revived->getMessage(), $revived->getCode());
	printf("sandbox after the trip: %s\n", var_export($revived->getSandbox(), true));
}

$sandbox->close();

?>
--EXPECT--
carries the sandbox: true
revived:  DevelopGravity\LuaExt\Exception\ConfigurationError: deliberate host misuse (code 7)
sandbox after the trip: NULL
