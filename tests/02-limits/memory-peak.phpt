--TEST--
stats()->peakMemoryBytes is a high-water mark, not the current usage
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Sandbox;

$sandbox = new Sandbox();

$usage = $sandbox->stats()->memoryBytes;
$peak = $sandbox->stats()->peakMemoryBytes;

var_dump($peak > 0);

// Strictly greater, and not by accident: growing a table's hash part allocates
// the replacement node vector while the old one is still live and only frees
// the old one afterwards (luaH_resize in ltable.c). Opening the standard
// libraries rehashes tables repeatedly, so every sandbox passes through a
// moment where it holds more than it ends up keeping. A "peak" that merely
// mirrored the current usage would fail here.
var_dump($peak > $usage);

// The mark is kept, not recomputed: reading it does not drag it back down to
// the current usage.
var_dump($sandbox->stats()->peakMemoryBytes === $peak);
var_dump($sandbox->stats()->memoryBytes === $usage);

// Nor does re-ceiling the sandbox disturb the history.
$sandbox->setLimits($sandbox->limits()->with(memoryBytes: 64 * 1024 * 1024));
var_dump($sandbox->stats()->peakMemoryBytes === $peak);

$sandbox->setLimits($sandbox->limits()->with(memoryBytes: null));
var_dump($sandbox->stats()->peakMemoryBytes === $peak);

$sandbox->close();

?>
--EXPECT--
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
