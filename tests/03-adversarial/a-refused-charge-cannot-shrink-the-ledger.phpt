--TEST--
A conversion charge the budget refuses is not discharged on the way out
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Exception\MemoryLimitError;
use DevelopGravity\LuaExt\Limits;
use DevelopGravity\LuaExt\OverflowBehavior;
use DevelopGravity\LuaExt\Sandbox;
use DevelopGravity\LuaExt\SandboxConfig;

// Converting a value for a host callback charges the memory ledger before the
// PHP copy is built, and the caller discharges everything it accumulated when
// the call unwinds. The accumulator used to count a REFUSED charge too, so the
// unwind handed back bytes never taken -- after one refused conversion the
// ledger under-reported real host memory by the refused amount, and every
// budget decision after that was made against a fiction.
//
// The observable: buffered output is real charged host memory. Park a megabyte
// there, force a refused conversion charge, and the megabyte must still be on
// the books afterwards.

$sandbox = new Sandbox(new SandboxConfig(
	limits: new Limits(
		memoryBytes: 16 * 1024 * 1024,
		outputBytes: 2 * 1024 * 1024,
		outputOverflow: OverflowBehavior::Fail,
	),
));

$sandbox->registerLibrary('host', [
	'take' => static function (string $payload): int {
		return strlen($payload);
	},
]);

// A megabyte of buffered output: real charged host memory that outlives the
// call, which is what makes it the witness here.
(void) $sandbox->eval('print(("p"):rep(1024 * 1024))', '=park-output');
(void) $sandbox->eval('collectgarbage("collect")', '=settle');

$parked = $sandbox->stats()->memoryBytes;

// A payload whose conversion charge cannot fit: 8 MiB in the Lua heap plus the
// parked output plus another 8 MiB for the PHP copy passes the 16 MiB ceiling.
try {
	(void) $sandbox->eval('return host.take(("x"):rep(8 * 1024 * 1024))', '=refused');
	echo "conversion => RAN\n";
} catch (MemoryLimitError) {
	echo "conversion => MemoryLimitError\n";
}

// Drop the script's own temporaries. What must remain is the parked output.
(void) $sandbox->eval('collectgarbage("collect") collectgarbage("collect")', '=resettle');

$remaining = $sandbox->stats()->memoryBytes;

// Measured on the unfixed tree the refusal took 2,097,249 bytes off the ledger
// -- the entire parked buffer -- because the discharge is clamped to whatever
// is charged, so bytes never taken are repaid out of somebody else's charge.
printf("ledger     => %s\n",
	$remaining >= 1024 * 1024
		? 'output still on the books'
		: sprintf('SHRUNK (%d of %d bytes left)', $remaining, $parked));

// The ceiling still holds afterwards, rather than being enforced against a
// deflated ledger.
try {
	(void) $sandbox->eval('return host.take(("y"):rep(8 * 1024 * 1024))', '=again');
	echo "again      => RAN\n";
} catch (MemoryLimitError) {
	echo "again      => MemoryLimitError\n";
}

$sandbox->close();

?>
--EXPECT--
conversion => MemoryLimitError
ledger     => output still on the books
again      => MemoryLimitError
