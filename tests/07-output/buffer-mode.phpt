--TEST--
Buffer mode accumulates, getOutput leaves it in place and takeOutput empties it
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Sandbox;

$sandbox = new Sandbox();

(void) $sandbox->eval('print("hello") print("world")');

// Buffer is the default, so this needed no configuration at all.
var_dump($sandbox->getOutput());
var_dump($sandbox->stats()->outputBytes);
var_dump($sandbox->stats()->outputTruncated);

// getOutput() leaves the buffer alone: reading twice reads the same thing.
var_dump($sandbox->getOutput() === $sandbox->getOutput());

// More output appends rather than replacing.
(void) $sandbox->eval('print("again")');
var_dump($sandbox->getOutput());

// takeOutput() hands the buffer over and resets the byte count with it.
var_dump($sandbox->takeOutput());
var_dump($sandbox->getOutput());
var_dump($sandbox->stats()->outputBytes);

// Nothing was dropped, so nothing claims it was.
var_dump($sandbox->stats()->outputTruncated);

// The sandbox goes on working after a take.
(void) $sandbox->eval('print("fresh")');
var_dump($sandbox->getOutput(), $sandbox->stats()->outputBytes);

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
