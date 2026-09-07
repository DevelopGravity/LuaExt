--TEST--
takeOutput() refills the output budget without rewinding stats()->outputBytes
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Limits;
use DevelopGravity\LuaExt\Sandbox;
use DevelopGravity\LuaExt\SandboxConfig;

// Draining the buffer used to zero the same counter stats() reports, so a
// host that drained in a loop -- the documented pattern -- erased its
// script's output total from the very snapshot it bills from. The budget and
// the stat are now separate counters: the drain refills the first and never
// touches the second.
$sandbox = new Sandbox(new SandboxConfig(
	limits: new Limits(outputBytes: 10),
));

(void) $sandbox->eval('io.write("12345678")', '=first');

var_dump($sandbox->stats()->outputBytes, $sandbox->takeOutput());

// The stat survives the drain...
var_dump($sandbox->stats()->outputBytes);

// ...while the budget really was handed back: another eight bytes fit under
// a ten-byte limit only because the first eight no longer count against it.
(void) $sandbox->eval('io.write("abcdefgh")', '=second');

var_dump($sandbox->stats()->outputBytes, $sandbox->getOutput());

$sandbox->close();

?>
--EXPECT--
int(8)
string(8) "12345678"
int(8)
int(16)
string(8) "abcdefgh"
