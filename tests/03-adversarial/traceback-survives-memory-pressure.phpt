--TEST--
A traceback is still captured when the memory budget is exhausted
--EXTENSIONS--
luaext
--XFAIL--
Needs Sandbox::eval(), registerLibrary() and the enforcing allocator; the traceback handler already lifts the ceiling for its own duration.
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Exception\FatalError;
use DevelopGravity\LuaExt\Exception\MemoryLimitError;
use DevelopGravity\LuaExt\Sandbox;

// Capturing a traceback means building a table, and building a table means
// allocating. A script that has spent its entire memory budget would therefore
// deny the host any account of where it failed -- the report of the failure
// failing for the same reason as the failure. The message handler runs with the
// ceiling lifted so that cannot happen, and puts it back afterwards.

$sandbox = new Sandbox();
$sandbox->registerLibrary('host', [
	// Leaves the script with no headroom at all, from inside the call.
	'exhaust' => static function () use ($sandbox): void {
		$sandbox->setMemoryLimit($sandbox->getMemoryUsage());
	},
]);

try {
	(void) $sandbox->eval(
		"local function inner() host.exhaust() error('raised with no budget left') end\n" .
		"local function outer() inner() end\n" .
		"outer()", '=pressure');
	echo "NOT THROWN\n";
} catch (Throwable $error) {
	$trace = $error->getLuaTrace() ?? [];

	printf("%s: %s\n", $error::class, $error->getMessage());
	printf("frames=%d innermost=%s outermost=%s chunk=%s line=%s\n",
		count($trace),
		$trace[0]['what'] ?? '-',
		$trace[count($trace) - 1]['what'] ?? '-',
		var_export($error->getChunkName(), true),
		var_export($error->getLuaLine(), true));
}

// And the ceiling is back: the lift lasts for the handler and not a moment
// longer, or a script could arrange to fail once and then allocate freely
// forever after.
try {
	(void) $sandbox->eval("local t = {} while true do t[#t + 1] = string.rep('x', 4096) end",
		'=after');
	echo "CEILING LEFT LIFTED\n";
} catch (MemoryLimitError $error) {
	echo "ceiling restored\n";
}

$sandbox->close();

// Breaching the ceiling itself is a separate matter. Lua raises its own memory
// error through a path that deliberately never runs a message handler -- it
// cannot afford to -- so the failure is reported with its class and message but
// without a stack. Recorded here so the absence is a known answer rather than a
// surprise.
$starved = new Sandbox();
$starved->setMemoryLimit(512 * 1024);

try {
	(void) $starved->eval("local t = {} while true do t[#t + 1] = string.rep('x', 4096) end",
		'=exhaust');
	echo "LIMIT ESCAPED\n";
} catch (MemoryLimitError $error) {
	printf("%s fatal=%s trace=%s\n", $error::class,
		var_export($error instanceof FatalError, true),
		var_export($error->getLuaTrace(), true));
}

?>
--EXPECT--
DevelopGravity\LuaExt\Exception\RuntimeError: pressure:1: raised with no budget left
frames=4 innermost=C outermost=main chunk='pressure' line=1
ceiling restored
DevelopGravity\LuaExt\Exception\MemoryLimitError fatal=true trace=NULL
