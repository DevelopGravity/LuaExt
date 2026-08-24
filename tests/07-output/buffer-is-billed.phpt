--TEST--
The output buffer is charged against memoryBytes like any other byte the script caused
--EXTENSIONS--
luaext
--XFAIL--
Needs print() from the library-policy wave. The charge, the discharge on takeOutput() and the refusal when the charge does not fit are wired and verified from the C side; what is missing is a Lua-visible way to reach luaext_output_write() -- the base library is still upstream's, so print() writes to the process's stdout instead of to the sandbox.
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Exception\FatalError;
use DevelopGravity\LuaExt\Limits;
use DevelopGravity\LuaExt\Sandbox;
use DevelopGravity\LuaExt\SandboxConfig;

// The buffer is host memory that lua_Alloc never sees. Without a charge against
// the same ceiling, a script that cannot allocate a large Lua string could
// still exhaust the budget by printing one -- so the hole is exactly the size
// of whatever a script is allowed to print.

// outputBytes lifted, so the only thing bounding this is memoryBytes.
$sandbox = new Sandbox(new SandboxConfig(limits: new Limits(outputBytes: 0)));

$before = $sandbox->getMemoryUsage();
(void) $sandbox->eval('print(string.rep("x", 200000))');
$after = $sandbox->getMemoryUsage();

var_dump($after - $before >= 200000);
var_dump($sandbox->getOutputLength());

// Handing the buffer over gives the memory back.
(void) $sandbox->takeOutput();
var_dump($sandbox->getMemoryUsage() < $after - 100000);

$sandbox->close();

// And a budget too small to hold the output refuses it rather than growing
// past the ceiling. The Lua string itself fits; three copies of it in the
// output buffer do not.
$tight = new Sandbox(new SandboxConfig(limits: new Limits(
	memoryBytes: 700000,
	outputBytes: 0,
)));

try {
	(void) $tight->eval(<<<'LUA'
		local line = string.rep("y", 300000)

		print(line)
		print(line)
		print(line)

		return "wrote it all"
	LUA, '=billing');
	echo "PRINTED PAST THE MEMORY CEILING\n";
} catch (FatalError $error) {
	printf("%s\n", $error::class);
}

var_dump($tight->getMemoryUsage() <= 700000);

$tight->close();

?>
--EXPECT--
bool(true)
int(200001)
bool(true)
DevelopGravity\LuaExt\Exception\OutputLimitError
bool(true)
