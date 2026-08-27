--TEST--
A sandbox constructs, closes idempotently, and refuses use afterwards
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);
use DevelopGravity\LuaExt\Exception\ClosedSandboxError;
use DevelopGravity\LuaExt\Sandbox;

$sandbox = new Sandbox();
var_dump($sandbox->isClosed());

// An explicit null config selects the same untrusted defaults.
$other = new Sandbox(null);
var_dump($other->isClosed());

$sandbox->close();
var_dump($sandbox->isClosed());

// Documented as idempotent: closing twice is not an error.
$sandbox->close();
var_dump($sandbox->isClosed());

try {
	(void) $sandbox->eval('return 1');
} catch (ClosedSandboxError $error) {
	printf("%s: %s\n", $error::class, $error->getMessage());

	// Nothing originated inside Lua, so there is no Lua context to report.
	var_dump($error->getLuaTrace(), $error->getLuaTraceAsString(),
		$error->getSandbox(), $error->getChunkName(), $error->getLuaLine());
}

// A sandbox owns an interpreter and a thread; a copy could only alias it.
try {
	$copy = clone $other;
} catch (Error $error) {
	printf("%s: %s\n", $error::class, $error->getMessage());
}

// The destructor closes whatever the host left open.
unset($other);
echo "done\n";

?>
--EXPECT--
bool(false)
bool(false)
bool(true)
bool(true)
DevelopGravity\LuaExt\Exception\ClosedSandboxError: The sandbox has been closed
NULL
string(0) ""
NULL
NULL
NULL
Error: Trying to clone an uncloneable object of class DevelopGravity\LuaExt\Sandbox
done
