--TEST--
The output buffer is charged against memoryBytes like any other byte the script caused
--EXTENSIONS--
luaext
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

$before = $sandbox->stats()->memoryBytes;
(void) $sandbox->eval('print(string.rep("x", 200000))');
$after = $sandbox->stats()->memoryBytes;

var_dump($after - $before >= 200000);
var_dump($sandbox->stats()->outputBytes);

// Handing the buffer over gives the memory back.
(void) $sandbox->takeOutput();
var_dump($sandbox->stats()->memoryBytes < $after - 100000);

$sandbox->close();

// And a budget too small to hold the output refuses it rather than growing
// past the ceiling. The Lua string itself fits; three copies of it in the
// output buffer do not.
//
// The refusal is a MEMORY error, and this line used to expect OutputLimitError:
// outputBytes is 0 here -- unbounded -- so "written all the output it is
// allowed" was never the truth about this refusal. What ran out is the memory
// budget, and the error now says so.
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

var_dump($tight->stats()->memoryBytes <= 700000);

$tight->close();

?>
--EXPECT--
bool(true)
int(200001)
bool(true)
DevelopGravity\LuaExt\Exception\MemoryLimitError
bool(true)
