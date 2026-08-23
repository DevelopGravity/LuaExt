--TEST--
Sandbox::setMemoryLimit() re-ceilings a live sandbox without unwinding it
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Sandbox;

$sandbox = new Sandbox();

$usage = $sandbox->getMemoryUsage();
$peak = $sandbox->getPeakMemoryUsage();

// Raising the ceiling is uneventful.
$sandbox->setMemoryLimit(64 * 1024 * 1024);
var_dump($sandbox->getMemoryUsage() === $usage);

// Null is the one spelling of "no ceiling".
$sandbox->setMemoryLimit(null);
var_dump($sandbox->getMemoryUsage() === $usage);

// The interesting case: a ceiling below what the sandbox already holds. This
// must not fail retroactively -- the memory is allocated and in use, and there
// is nothing safe to unwind. Only the next allocation that would grow the heap
// is refused, which nothing here attempts.
$sandbox->setMemoryLimit(1024);
var_dump($sandbox->getMemoryUsage() === $usage);
var_dump($sandbox->getPeakMemoryUsage() === $peak);

// Down to a single byte, and the sandbox is still perfectly usable.
$sandbox->setMemoryLimit(1);
var_dump($sandbox->getMemoryUsage() === $usage);
var_dump($sandbox->isClosed());

// ...and still closable, which matters most: a sandbox held below its own
// footprint must not become impossible to tear down.
$sandbox->close();
var_dump($sandbox->isClosed());

// Zero would be a second spelling of "no ceiling" if it were accepted, and a
// limit no allocation could satisfy if it were taken literally. Neither is
// likely to be what a caller meant.
$live = new Sandbox();

foreach ([0, -1, PHP_INT_MIN] as $bytes) {
	try {
		$live->setMemoryLimit($bytes);
	} catch (ValueError $error) {
		printf("%s: %s\n", $error::class, $error->getMessage());
	}
}

// A rejected argument changes nothing.
var_dump($live->getMemoryUsage() > 0);

$live->close();

?>
--EXPECT--
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(false)
bool(true)
ValueError: DevelopGravity\LuaExt\Sandbox::setMemoryLimit(): Argument #1 ($bytes) must be greater than 0, or null to lift the limit
ValueError: DevelopGravity\LuaExt\Sandbox::setMemoryLimit(): Argument #1 ($bytes) must be greater than 0, or null to lift the limit
ValueError: DevelopGravity\LuaExt\Sandbox::setMemoryLimit(): Argument #1 ($bytes) must be greater than 0, or null to lift the limit
bool(true)
