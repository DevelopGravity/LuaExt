--TEST--
Buffer mode accumulates, getOutput leaves it in place and takeOutput empties it
--EXTENSIONS--
luaext
--XFAIL--
Needs print() from the library-policy wave. The sink, the buffer and all four Sandbox methods are wired and verified from the C side; what is missing is a Lua-visible way to reach luaext_output_write() -- the base library is still upstream's, so print() writes to the process's stdout instead of to the sandbox.
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Sandbox;

$sandbox = new Sandbox();

(void) $sandbox->eval('print("hello") print("world")');

// Buffer is the default, so this needed no configuration at all.
var_dump($sandbox->getOutput());
var_dump($sandbox->getOutputLength());
var_dump($sandbox->isOutputTruncated());

// getOutput() leaves the buffer alone: reading twice reads the same thing.
var_dump($sandbox->getOutput() === $sandbox->getOutput());

// More output appends rather than replacing.
(void) $sandbox->eval('print("again")');
var_dump($sandbox->getOutput());

// takeOutput() hands the buffer over and resets the byte count with it.
var_dump($sandbox->takeOutput());
var_dump($sandbox->getOutput());
var_dump($sandbox->getOutputLength());

// Nothing was dropped, so nothing claims it was.
var_dump($sandbox->isOutputTruncated());

// The sandbox goes on working after a take.
(void) $sandbox->eval('print("fresh")');
var_dump($sandbox->getOutput(), $sandbox->getOutputLength());

$sandbox->close();

?>
--EXPECT--
string(12) "hello
world
"
int(12)
bool(false)
bool(true)
string(18) "hello
world
again
"
string(18) "hello
world
again
"
string(0) ""
int(0)
bool(false)
string(6) "fresh
"
int(6)
