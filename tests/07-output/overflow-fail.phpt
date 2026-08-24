--TEST--
OverflowBehavior::Fail stops the script with an OutputLimitError at the cap
--EXTENSIONS--
luaext
--XFAIL--
Needs print() from the library-policy wave. The refusal, the retained prefix and the OutputLimitError classification are wired and verified from the C side; what is missing is a Lua-visible way to reach luaext_output_write() -- the base library is still upstream's, so print() writes to the process's stdout instead of to the sandbox.
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Exception\FatalError;
use DevelopGravity\LuaExt\Exception\OutputLimitError;
use DevelopGravity\LuaExt\Limits;
use DevelopGravity\LuaExt\OverflowBehavior;
use DevelopGravity\LuaExt\Sandbox;
use DevelopGravity\LuaExt\SandboxConfig;

// Fail is the documented default, so this is what an unconfigured overflow does.
var_dump((new Limits())->outputOverflow === OverflowBehavior::Fail);

$sandbox = new Sandbox(new SandboxConfig(limits: new Limits(outputBytes: 10)));

// Exactly at the cap is not a breach.
(void) $sandbox->eval('print("012345678")');
var_dump($sandbox->getOutput(), $sandbox->isOutputTruncated());

try {
	(void) $sandbox->eval('print("one byte too many") print("never reached")');
	echo "RAN PAST THE OUTPUT BUDGET\n";
} catch (OutputLimitError $error) {
	// A limit breach is a FatalError, not a RuntimeError: the host hierarchy
	// carries the same catchable-versus-fatal split the interpreter does.
	printf("%s fatal=%s\n", $error::class, var_export($error instanceof FatalError, true));
}

// What was accepted before the cap is still the script's output and is kept.
var_dump($sandbox->getOutput());

// The breach is recorded as well as raised, so a host that caught the error can
// still tell that output was lost.
var_dump($sandbox->isOutputTruncated());
var_dump($sandbox->stats()->outputTruncated);

$sandbox->close();

?>
--EXPECT--
bool(true)
string(10) "012345678
"
bool(false)
DevelopGravity\LuaExt\Exception\OutputLimitError fatal=true
string(10) "012345678
"
bool(true)
bool(true)
