--TEST--
A write refused by the memory budget raises MemoryLimitError, not OutputLimitError
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Limits;
use DevelopGravity\LuaExt\Sandbox;
use DevelopGravity\LuaExt\SandboxConfig;

// Two budgets can refuse an output write: outputBytes, and -- because the
// buffer is host memory billed like anything else the script causes -- the
// memory ceiling. They used to share one `false` on the way out of the sink,
// so the caller had one message for both and every memory refusal was reported
// as "The sandbox has written all the output it is allowed".
//
// That sentence was not merely vague, it was false, and it was seen in the
// wild: a host debugging an OutputLimitError against a 1 MiB outputBytes when
// only kilobytes had been printed. The number that had actually run out was
// memoryBytes, and no amount of raising outputBytes would have fixed it.
//
// The first sandbox reproduces exactly that shape: outputBytes is a limit the
// script comes nowhere near, memoryBytes is the one that fills.

$sandbox = new Sandbox(new SandboxConfig(
	limits: new Limits(memoryBytes: 65536, outputBytes: 1048576),
));

try {
	(void) $sandbox->eval('for i = 1, 200 do print(string.rep("x", 256)) end', '=fill');
	echo "PRINTED PAST THE MEMORY CEILING\n";
} catch (Throwable $error) {
	printf("memory-bound refusal: %s\n", $error::class);
}

printf("output emitted was far under outputBytes: %s\n",
	$sandbox->stats()->outputBytes < 1048576 / 2 ? 'yes' : 'no');

$sandbox->close();

// The other budget still answers with its own error: plenty of memory, a tiny
// outputBytes, and the same message as always.
$small = new Sandbox(new SandboxConfig(
	limits: new Limits(memoryBytes: 33554432, outputBytes: 4096),
));

try {
	(void) $small->eval('for i = 1, 200 do print(string.rep("x", 256)) end', '=overflow');
	echo "PRINTED PAST THE OUTPUT BUDGET\n";
} catch (Throwable $error) {
	printf("output-bound refusal: %s\n", $error::class);
}

$small->close();

?>
--EXPECT--
memory-bound refusal: DevelopGravity\LuaExt\Exception\MemoryLimitError
output emitted was far under outputBytes: yes
output-bound refusal: DevelopGravity\LuaExt\Exception\OutputLimitError
