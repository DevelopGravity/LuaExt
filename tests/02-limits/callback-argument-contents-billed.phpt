--TEST--
A callback argument's converted contents are billed against the memory limit
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

/*
 * Converting a Lua value for a host callback allocates a SECOND copy on the PHP
 * side. lua_Alloc bills the Lua original and nothing billed the duplicate, so a
 * sandbox capped at N bytes transiently held appreciably more than N -- and a
 * table is worse than a string, because a PHP array costs more per element than
 * a Lua table does.
 *
 * The charge has to be given back when the params are released, or a sandbox
 * would lose budget on every callback until it could no longer run anything.
 * That is the half worth testing hardest: an over-eager charge is a slow death
 * rather than an obvious failure.
 */

use DevelopGravity\LuaExt\Exception\MemoryLimitError;
use DevelopGravity\LuaExt\Limits;
use DevelopGravity\LuaExt\Sandbox;
use DevelopGravity\LuaExt\SandboxConfig;

/* ---------------------------------------------------------------------------
 * The charge is released, so repeated calls do not erode the budget
 * ------------------------------------------------------------------------ */

$sandbox = new Sandbox(new SandboxConfig(
	limits: (new Limits())->with(memoryBytes: 8 * 1024 * 1024),
));

$seen = 0;

$sandbox->registerLibrary('host', [
	'take' => static function (string $text) use (&$seen): int {
		$seen++;

		return strlen($text);
	},
]);

// Each call converts ~256 KiB. Thirty-two of them is 8 MiB of cumulative
// conversion against an 8 MiB limit: fine if the charge is discharged, fatal by
// about the halfway point if it is not.
$total = $sandbox->eval(<<<'LUA'
	local chunk = string.rep("x", 256 * 1024)
	local total = 0

	for _ = 1, 32 do
		total = total + host.take(chunk)
	end

	return total
LUA, '=repeat');

var_dump($seen === 32);
var_dump($total[0] === 32 * 256 * 1024);

// The budget is where it started, not 8 MiB poorer.
var_dump($sandbox->stats()->memoryBytes < 2 * 1024 * 1024);

$sandbox->close();

/* ---------------------------------------------------------------------------
 * An argument too large for the remaining budget is refused
 * ------------------------------------------------------------------------ */

$tight = new Sandbox(new SandboxConfig(
	limits: (new Limits())->with(memoryBytes: 4 * 1024 * 1024),
));

$reached = false;

$tight->registerLibrary('host', [
	'take' => static function (string $text) use (&$reached): void {
		$reached = true;
	},
]);

try {
	// The Lua string fits; the PHP copy of it does not.
	(void) $tight->eval(<<<'LUA'
		local chunk = string.rep("y", 3 * 1024 * 1024)
		host.take(chunk)
	LUA, '=toobig');

	echo "NOT REFUSED\n";
} catch (MemoryLimitError) {
	echo "refused the copy\n";
}

// The callback must never have run: the refusal happens while converting its
// arguments, before anything is called.
var_dump($reached);

$tight->close();

?>
--EXPECT--
bool(true)
bool(true)
bool(true)
refused the copy
bool(false)
