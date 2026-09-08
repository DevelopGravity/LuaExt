--TEST--
dump() honours Limits::$maxStringLength instead of growing host memory unbounded
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Capabilities;
use DevelopGravity\LuaExt\Exception\RuntimeError;
use DevelopGravity\LuaExt\Limits;
use DevelopGravity\LuaExt\Sandbox;
use DevelopGravity\LuaExt\SandboxConfig;

$capabilities = (new Capabilities())->with(dumpBytecode: true);

// A ceiling far below any real dump: the writer must give up and report,
// never longjmp out of lua_dump and never keep buffering past the limit --
// the same ceiling every other way of materialising a string already honours.
$bounded = new Sandbox(new SandboxConfig(
	capabilities: $capabilities,
	limits: new Limits(maxStringLength: 64),
));
$function = $bounded->compile('local a, b = ... return a * b + 1', '@math.lua');

try {
	$function->dump();
	echo "NOT REFUSED\n";
} catch (RuntimeError $error) {
	echo $error->getMessage(), "\n";
}

// The sandbox survives the refusal, and an unlimited ceiling still dumps.
var_dump($function->call(6, 7)[0]);
$bounded->close();

$unlimited = new Sandbox(new SandboxConfig(
	capabilities: $capabilities,
	limits: new Limits(maxStringLength: 0),
));
$dump = $unlimited->compile('local a, b = ... return a * b + 1', '@math.lua')->dump();
var_dump(substr($dump, 0, 4) === 'LXBC');
$unlimited->close();

?>
--EXPECT--
This function's bytecode dump exceeds Limits::$maxStringLength (64 bytes)
int(43)
bool(true)
