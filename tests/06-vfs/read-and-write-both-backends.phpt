--TEST--
The same script reads and writes identically whether the backend can seek or not
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

require __DIR__ . '/memory-filesystem.inc';

use DevelopGravity\LuaExt\Capabilities;
use DevelopGravity\LuaExt\Sandbox;
use DevelopGravity\LuaExt\SandboxConfig;

// A plain FileSystem is buffered whole-file; a RangedFileSystem is streamed
// through readRange/writeRange. Those are two separate code paths, and the
// whole point of the split is that a script cannot tell which one it got.
//
// So the script below is run twice, unchanged, and the outputs are compared.

$script = <<<'LUA'
	local handle = assert(io.open("/notes.txt", "r"))
	local first = handle:read("l")
	local rest = handle:read("a")
	handle:close()

	local out = assert(io.open("/out.txt", "w"))
	out:write("first=", first, "\n")
	out:write("rest=", rest, "\n")
	out:close()

	local back = assert(io.open("/out.txt", "r"))
	local all = back:read("a")
	back:close()

	return all
LUA;

$seed = ['/notes.txt' => "alpha\nbeta\ngamma"];

$results = [];

foreach (['buffered' => MemoryFileSystem::class, 'ranged' => RangedMemoryFileSystem::class] as $label => $class) {
	$backend = new $class($seed);

	$sandbox = new Sandbox(new SandboxConfig(
		capabilities: (new Capabilities())->with(vfs: true, vfsWrite: true),
		filesystem: $backend,
	));

	$results[$label] = $sandbox->eval($script, '=vfs')[0];

	printf("%-8s stored: %s\n", $label, var_export($backend->files['/out.txt'], true));

	$sandbox->close();
}

var_dump($results['buffered'] === $results['ranged']);
echo $results['buffered'];

?>
--EXPECT--
buffered stored: 'first=alpha
rest=beta
gamma
'
ranged   stored: 'first=alpha
rest=beta
gamma
'
bool(true)
first=alpha
rest=beta
gamma
