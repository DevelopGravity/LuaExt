--TEST--
Line reads keep the interpreter stack balanced across chunk boundaries and multi-format iterators
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

require __DIR__ . '/memory-filesystem.inc';

use DevelopGravity\LuaExt\Capabilities;
use DevelopGravity\LuaExt\Sandbox;
use DevelopGravity\LuaExt\SandboxConfig;
use DevelopGravity\LuaExt\VfsQuota;

// Two stack-arithmetic faults lived here, and neither one shows up as a wrong
// answer in a release build -- Lua's api_check needs LUA_USE_APICHECK, which
// only --enable-luaext-debug defines. So this file asserts the observable
// behaviour, and the debug and sanitizer legs are what turn the same cases into
// hard failures. Do not "fix" a failure here by dropping those legs.
//
//   1. A ranged line read accumulates through a luaL_Buffer while each chunk
//      also sits on the stack as a Lua string. Once the buffer outgrows its
//      inline array it keeps a box on the stack too, and popping "the chunk"
//      popped the box instead -- the buffer's own storage, freed while it was
//      still being written through. Needs a line longer than one chunk.
//
//   2. The lines() iterator reserved room for its formats but not for the
//      results read on top of them, so three or more formats pushed past the
//      allocation.

$longLine = str_repeat('a', 4000);

$files = [
	'/long.txt' => $longLine . "\n" . "second line\n" . str_repeat('b', 3000) . "\n",
	'/short.txt' => "one\ntwo\nthree\nfour\nfive\nsix\n",
];

$open = static fn (object $backend): Sandbox => new Sandbox(new SandboxConfig(
	capabilities: (new Capabilities())->with(vfs: true),
	filesystem: $backend,
	vfsQuota: new VfsQuota(maxOperations: 10000),
));

// A line several chunks long, read back through a seekable backend: the whole
// line must survive, byte for byte.
$ranged = $open(new RangedMemoryFileSystem($files));

[$length, $matches, $second] = $ranged->eval(<<<'LUA'
	local f = io.open("/long.txt", "r")
	local first = f:read("l")
	local second = f:read("l")
	f:close()

	return #first, first == ("a"):rep(4000), second
	LUA, '=long-line');

printf("ranged long line  => %d bytes, intact: %s, next line: %s\n",
	$length, $matches ? 'yes' : 'NO', $second);

// The same file through a backend that cannot seek, so the extension buffers
// the whole thing instead: both paths must agree.
$buffered = $open(new MemoryFileSystem($files));

[$bufferedLength, $bufferedMatches] = $buffered->eval(<<<'LUA'
	local f = io.open("/long.txt", "r")
	local first = f:read("l")
	f:close()

	return #first, first == ("a"):rep(4000)
	LUA, '=long-line-buffered');

printf("buffered long line => %d bytes, intact: %s\n",
	$bufferedLength, $bufferedMatches ? 'yes' : 'NO');

// Keeping the newline is the other branch of the same partial-take arithmetic.
[$withNewline] = $ranged->eval(<<<'LUA'
	local f = io.open("/long.txt", "r")
	local first = f:read("L")
	f:close()

	return #first
	LUA, '=keep-newline');

printf("read(\"L\")          => %d bytes\n", $withNewline);

// An iterator with more formats than the old reservation allowed. Each round
// pushes one result per format on top of the formats themselves.
[$rows] = $ranged->eval(<<<'LUA'
	local collected = {}

	for a, b, c, d in io.lines("/short.txt", "l", "l", "l", "l") do
		collected[#collected + 1] = table.concat({a, b, c, d}, ",")
	end

	return table.concat(collected, " | ")
	LUA, '=four-formats');

printf("four formats      => %s\n", $rows);

// The documented ceiling, exercised at its edge rather than near it.
[$wide] = $ranged->eval(<<<'LUA'
	local formats = {}

	for index = 1, 200 do
		formats[index] = "l"
	end

	local iterator = io.lines("/short.txt", table.unpack(formats))
	local first = iterator()

	return first
	LUA, '=two-hundred-formats');

printf("200 formats       => %s\n", $wide);

$ranged->close();
$buffered->close();

?>
--EXPECT--
ranged long line  => 4000 bytes, intact: yes, next line: second line
buffered long line => 4000 bytes, intact: yes
read("L")          => 4001 bytes
four formats      => one,two,three,four | five,six
200 formats       => one
