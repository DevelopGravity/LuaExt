--TEST--
Discard mode keeps nothing but still counts what the script emitted
--EXTENSIONS--
luaext
--XFAIL--
Needs print() from the library-policy wave. Discard's accounting is wired and verified from the C side; what is missing is a Lua-visible way to reach luaext_output_write() -- the base library is still upstream's, so print() writes to the process's stdout instead of to the sandbox.
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Limits;
use DevelopGravity\LuaExt\OutputMode;
use DevelopGravity\LuaExt\OverflowBehavior;
use DevelopGravity\LuaExt\Sandbox;
use DevelopGravity\LuaExt\SandboxConfig;

$sandbox = new Sandbox(new SandboxConfig(outputMode: OutputMode::Discard));

(void) $sandbox->eval('print("thrown away") print("also thrown away")');

// Nothing is kept, and asking for it is not an error.
var_dump($sandbox->getOutput());
var_dump($sandbox->takeOutput());

// The bytes are still counted. Limits::$outputBytes is a statement about how
// much a script may emit, not about where it lands, so a host can still see
// that a Discard-mode script is shouting.
var_dump($sandbox->getOutputLength());
var_dump($sandbox->stats()->outputBytes);
var_dump($sandbox->isOutputTruncated());

$sandbox->close();

// And the budget applies in Discard mode too, for the same reason.
$capped = new Sandbox(new SandboxConfig(
	limits: new Limits(outputBytes: 4, outputOverflow: OverflowBehavior::Truncate),
	outputMode: OutputMode::Discard,
));

(void) $capped->eval('print("far too much")');

var_dump($capped->isOutputTruncated());
var_dump($capped->getOutput());
var_dump($capped->stats()->outputTruncated);

$capped->close();

?>
--EXPECT--
string(0) ""
string(0) ""
int(29)
int(29)
bool(false)
bool(true)
string(0) ""
bool(true)
