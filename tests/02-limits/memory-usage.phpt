--TEST--
stats()->memoryBytes reports the bytes the interpreter really holds
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Sandbox;

// Building an interpreter and opening five libraries is itself a substantial
// Lua workload, so a sandbox that has done nothing else still has a real, and
// far from trivial, heap behind it. A placeholder allocator that only forwarded
// to realloc would report zero here.
$sandbox = new Sandbox();
$usage = $sandbox->stats()->memoryBytes;

var_dump($usage > 0);

// Loose enough to survive a different pointer size or a future stdlib change,
// tight enough that a counter tracking something other than bytes -- a type
// tag mistaken for an old size, say -- would fall outside it.
var_dump($usage > 4096 && $usage < 1024 * 1024);

// Reading the counters is not itself an allocation.
var_dump($sandbox->stats()->memoryBytes === $usage);
var_dump($sandbox->stats()->memoryBytes === $usage);

// Each sandbox owns its own heap and its own counters; the accounting is
// per-state, not per-process.
$other = new Sandbox();
var_dump($other->stats()->memoryBytes > 0);
var_dump($sandbox->stats()->memoryBytes === $usage);

// Closing one leaves the other's accounting untouched.
$other->close();
var_dump($sandbox->stats()->memoryBytes === $usage);

$sandbox->close();

?>
--EXPECT--
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
