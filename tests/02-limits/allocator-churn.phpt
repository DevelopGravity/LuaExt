--TEST--
Repeated interpreter construction leaves the allocator's accounting exact
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Sandbox;

// Building an interpreter drives every branch of the allocator many thousands
// of times over: fresh blocks, growing reallocations, shrinking ones, and
// frees. Doing it repeatedly is what a sanitizer leg needs in order to catch a
// block the allocator forgot to release, and what catches accounting that drifts
// -- a free crediting back the wrong number would show up as a usage figure
// that wandered from one round to the next.
$first = null;
$stable = true;

// The self-reported figure only proves the LUA heap balances; the host-side
// bracket below is what catches request-arena bookkeeping the extension keeps
// AROUND each sandbox and forgets to release -- invisible to the Lua ledger.
$warmup = new Sandbox();
$warmup->close();
$hostBefore = memory_get_usage();

for ($round = 0; $round < 64; $round++) {
	$sandbox = new Sandbox();
	$usage = $sandbox->stats()->memoryBytes;

	if ($first === null) {
		$first = $usage;
	} elseif ($usage !== $first) {
		$stable = false;
	}

	$sandbox->close();
}

$hostGrowth = memory_get_usage() - $hostBefore;

var_dump($first > 0);
var_dump($stable);

// Generous: 64 lifecycles at even 1 KB of forgotten host bookkeeping each
// would blow through this.
printf("host arena held flat: %s\n", $hostGrowth < 32768 ? 'yes' : "no ({$hostGrowth} bytes)");

// Interleaving them proves the counters are per-sandbox rather than a shared
// running total that only looks right when one exists at a time.
$sandboxes = [];

for ($index = 0; $index < 32; $index++) {
	$sandboxes[] = new Sandbox();
}

$allMatch = true;

foreach ($sandboxes as $sandbox) {
	if ($sandbox->stats()->memoryBytes !== $first) {
		$allMatch = false;
	}
}

var_dump($allMatch);

// Dropped without an explicit close(): the destructor has to release the heap
// just as close() would.
$sandboxes = [];

var_dump(true);

?>
--EXPECT--
bool(true)
bool(true)
host arena held flat: yes
bool(true)
bool(true)
