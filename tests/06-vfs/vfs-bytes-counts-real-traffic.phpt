--TEST--
stats()->vfsBytes counts the bytes that actually crossed to the backend, both directions
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

require __DIR__ . '/memory-filesystem.inc';

use DevelopGravity\LuaExt\Capabilities;
use DevelopGravity\LuaExt\Sandbox;
use DevelopGravity\LuaExt\SandboxConfig;

// vfsBytes is a byte meter, independent of vfsOperations: a hundred one-byte
// operations and one hundred-byte operation must read differently. Reads
// count what the backend handed over; writes count what it was handed.
$sandbox = new Sandbox(new SandboxConfig(
	capabilities: (new Capabilities())->with(vfs: true, vfsWrite: true),
	filesystem: new MemoryFileSystem(['/in.txt' => '0123456789']),
));

var_dump($sandbox->stats()->vfsBytes);

// Reading the ten-byte file, whole.
(void) $sandbox->eval('
	local handle = assert(io.open("/in.txt", "r"))
	local everything = handle:read("a")
	handle:close()
	assert(#everything == 10)
', '=reads');

$afterRead = $sandbox->stats()->vfsBytes;
var_dump($afterRead);

// Writing eight bytes back out.
(void) $sandbox->eval('
	local handle = assert(io.open("/out.txt", "w"))
	handle:write("abcdefgh")
	handle:close()
', '=writes');

var_dump($sandbox->stats()->vfsBytes - $afterRead);

// The meter never moves without traffic: exists() is an operation with no
// payload.
$flat = $sandbox->stats()->vfsBytes;
(void) $sandbox->eval('assert(io.open("/absent.txt", "r") == nil)', '=probe');
var_dump($sandbox->stats()->vfsBytes - $flat);

$sandbox->close();

?>
--EXPECT--
int(0)
int(10)
int(8)
int(0)
