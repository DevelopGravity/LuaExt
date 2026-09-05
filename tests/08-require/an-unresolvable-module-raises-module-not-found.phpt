--TEST--
require() of a name nothing resolves raises a catchable ModuleNotFoundError
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Capabilities;
use DevelopGravity\LuaExt\Exception\ModuleNotFoundError;
use DevelopGravity\LuaExt\Sandbox;
use DevelopGravity\LuaExt\SandboxConfig;

// A sandbox with the require capability but no preloads, no filesystem and no
// resolver: every rung of the resolution ladder answers "not mine", and the
// bottom is a dedicated class rather than a generic RuntimeError, because "the
// module does not exist" is a condition a host routinely branches on.

$sandbox = new Sandbox(new SandboxConfig(
	capabilities: (new Capabilities())->with(require: true),
));

// Host side: the class, and the message naming the module.
try {
	(void) $sandbox->eval('return require("no.such.module")', '=missing');
} catch (ModuleNotFoundError $error) {
	printf("host  => %s: %s\n", substr(strrchr($error::class, '\\'), 1), $error->getMessage());
}

// Script side: catchable, unlike a limit or a withheld feature. A script that
// wants to fall back when an optional module is absent may.
$results = $sandbox->eval(<<<'LUA'
	-- The caught value is the extension's own error userdata (that is what
	-- makes it unforgeable); tostring() is the supported way to read it.
	local ok, message = pcall(require, "no.such.module")
	return ok, tostring(message)
	LUA, '=pcall-missing');
printf("pcall => ok=%s message=%s\n", var_export($results[0], true), $results[1]);

// A failed resolution is not cached: the same name asked again reports the
// same answer rather than a poisoned cache entry.
try {
	(void) $sandbox->eval('return require("no.such.module")', '=again');
} catch (ModuleNotFoundError $error) {
	printf("again => %s\n", $error->getMessage());
}

$sandbox->close();

?>
--EXPECT--
host  => ModuleNotFoundError: Module "no.such.module" was not found
pcall => ok=false message=Module "no.such.module" was not found
again => Module "no.such.module" was not found
