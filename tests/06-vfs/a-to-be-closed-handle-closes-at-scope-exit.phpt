--TEST--
A to-be-closed file handle flushes and closes at scope exit, success or error
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

require __DIR__ . '/memory-filesystem.inc';

use DevelopGravity\LuaExt\Capabilities;
use DevelopGravity\LuaExt\Sandbox;
use DevelopGravity\LuaExt\SandboxConfig;

// `local f <close> = io.open(...)` is the idiomatic 5.4/5.5 way to guarantee a
// handle is released on every exit path, and stock Lua's file metatable
// carries the __close that makes it legal. Without one, the declaration
// itself raises "variable 'f' got a non-closable value" -- a divergence from
// the language this sandbox embeds.
$filesystem = new MemoryFileSystem();

$sandbox = new Sandbox(new SandboxConfig(
	capabilities: (new Capabilities())->with(vfs: true, vfsWrite: true),
	filesystem: $filesystem,
));

// The flush happens AT the scope exit, not at the end-of-call sweep: the
// read-back runs inside the same eval(), after the block but before any sweep
// could have closed anything.
$results = $sandbox->eval(<<<'LUA'
	do
		local f <close> = assert(io.open("/note.txt", "w"))
		f:write("first line")
	end

	local g = assert(io.open("/note.txt", "r"))
	local written = g:read("a")
	g:close()

	return written
LUA, '=scope-exit');

var_dump($results[0]);

// The error path is the reason <close> exists: the scope unwinds through a
// caught script error and the handle still flushed before pcall returned.
$results = $sandbox->eval(<<<'LUA'
	local ok = pcall(function()
		local f <close> = assert(io.open("/err.txt", "w"))
		f:write("kept despite the error")
		error("scope exits sideways")
	end)

	local g = assert(io.open("/err.txt", "r"))
	local written = g:read("a")
	g:close()

	return ok, written
LUA, '=error-exit');

var_dump($results[0]);
var_dump($results[1]);

// An explicit close before the scope ends is not a double release: the
// __close that follows finds a closed handle and reports success, as
// :close() itself does.
$results = $sandbox->eval(<<<'LUA'
	do
		local f <close> = assert(io.open("/note.txt", "r"))
		f:close()
	end

	return "no double release"
LUA, '=already-closed');

var_dump($results[0]);

$sandbox->close();

?>
--EXPECT--
string(10) "first line"
bool(false)
string(22) "kept despite the error"
string(17) "no double release"
