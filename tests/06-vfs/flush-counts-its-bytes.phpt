--TEST--
stats()->vfsBytes counts the bytes file:flush() hands the backend, and close() does not recount them
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

require __DIR__ . '/memory-filesystem.inc';

use DevelopGravity\LuaExt\Capabilities;
use DevelopGravity\LuaExt\Sandbox;
use DevelopGravity\LuaExt\SandboxConfig;

// flush() sends the whole buffer to FileSystem::write() and clears the dirty
// flag, so close() will not write again. The byte counter has to move where
// the bytes move: a script that flushed used to leave stats()->vfsBytes at
// zero while the backend held the full payload.
$filesystem = new MemoryFileSystem();

$sandbox = new Sandbox(new SandboxConfig(
	capabilities: (new Capabilities())->with(vfs: true, vfsWrite: true),
	filesystem: $filesystem,
));

(void) $sandbox->eval('
	local handle = assert(io.open("/payload.txt", "w"))
	handle:write("12345")
	handle:flush()
	-- A second flush with nothing new buffered moves nothing and counts
	-- nothing; close() sees a clean handle and writes nothing either.
	handle:flush()
	handle:close()
', '=flushed');

var_dump($filesystem->files['/payload.txt'], $sandbox->stats()->vfsBytes);

// The close path keeps counting what IT writes: bytes buffered after the
// flush are moved (and counted) exactly once, by close().
(void) $sandbox->eval('
	local handle = assert(io.open("/more.txt", "w"))
	handle:write("abc")
	handle:flush()
	handle:write("def")
	handle:close()
', '=both');

var_dump($filesystem->files['/more.txt'], $sandbox->stats()->vfsBytes);

$sandbox->close();

?>
--EXPECT--
string(5) "12345"
int(5)
string(6) "abcdef"
int(14)
