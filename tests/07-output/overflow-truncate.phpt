--TEST--
OverflowBehavior::Truncate drops the excess, records it, and lets the script run on
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Limits;
use DevelopGravity\LuaExt\OverflowBehavior;
use DevelopGravity\LuaExt\Sandbox;
use DevelopGravity\LuaExt\SandboxConfig;

$sandbox = new Sandbox(new SandboxConfig(limits: new Limits(
	outputBytes: 10,
	outputOverflow: OverflowBehavior::Truncate,
)));

// Reaching the cap exactly is not an overflow: outputBytes is how much a script
// MAY emit, so only the byte past it is refused.
var_dump($sandbox->eval('print("012345678") return "ran to the end"'));
var_dump($sandbox->getOutput());
var_dump($sandbox->isOutputTruncated());

// Past it, the excess is dropped and the script is not told. That is what
// Truncate means, and it is why the flag exists.
var_dump($sandbox->eval('print("more") return "still running"'));
var_dump($sandbox->getOutput());
var_dump($sandbox->isOutputTruncated());

// What the script EMITTED, not what survived: a byte count that shrank when the
// sink dropped the excess would tell a host its script behaved.
var_dump($sandbox->getOutputLength());
var_dump($sandbox->stats()->outputBytes, $sandbox->stats()->outputTruncated);

// Taking the output empties the buffer and resets the count -- but the
// truncation flag stays, because a host that took the output still needs to
// know it was incomplete.
var_dump($sandbox->takeOutput());
var_dump($sandbox->getOutput());
var_dump($sandbox->getOutputLength());
var_dump($sandbox->isOutputTruncated());
var_dump($sandbox->stats()->outputTruncated);

$sandbox->close();

?>
--EXPECT--
array(1) {
  [0]=>
  string(14) "ran to the end"
}
string(10) "012345678
"
bool(false)
array(1) {
  [0]=>
  string(13) "still running"
}
string(10) "012345678
"
bool(true)
int(15)
int(15)
bool(true)
string(10) "012345678
"
string(0) ""
int(0)
bool(true)
bool(true)
