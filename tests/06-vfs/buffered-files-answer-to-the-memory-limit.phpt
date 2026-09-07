--TEST--
Buffered file contents are charged against memoryBytes, and refunded on close
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

require __DIR__ . '/memory-filesystem.inc';

use DevelopGravity\LuaExt\Capabilities;
use DevelopGravity\LuaExt\Exception\MemoryLimitError;
use DevelopGravity\LuaExt\Limits;
use DevelopGravity\LuaExt\Sandbox;
use DevelopGravity\LuaExt\SandboxConfig;
use DevelopGravity\LuaExt\VfsQuota;

// A buffered handle holds the whole file host-side, and that buffer is the
// sandbox's memory as much as a Lua string is -- the docs have promised as
// much all along. VfsQuota::$maxTotalBytes alone let eight megabytes of file
// buffers sit inside a one-megabyte sandbox, invisible to memoryBytes and to
// stats(). Also covered: the read-it-all path on a ranged backend with
// maxFileBytes lifted, whose "everything" length used to narrow to -1 on its
// way to readRange().

$files = [
	'/big.bin' => str_repeat('B', 4 * 1024 * 1024),
	'/small.txt' => str_repeat('s', 256 * 1024),
	'/tiny.txt' => str_repeat('t', 100),
];

// A 4 MiB file cannot be buffered inside a 1 MiB memory budget, however
// generous the VFS quota is.
$cramped = new Sandbox(new SandboxConfig(
	capabilities: (new Capabilities())->with(vfs: true),
	filesystem: new MemoryFileSystem($files),
	limits: new Limits(memoryBytes: 1024 * 1024),
	vfsQuota: new VfsQuota(maxTotalBytes: 16 * 1024 * 1024, maxFileBytes: 0),
));

try {
	(void) $cramped->eval('return io.open("/big.bin", "r")', '=too-big');
	echo "open big  => OPENED (unbilled)\n";
} catch (MemoryLimitError) {
	echo "open big  => MemoryLimitError\n";
}

$cramped->close();

// Within budget the charge shows in stats while the handle is open -- observed
// from a host callback, since handles are call-scoped and the sweep has
// refunded everything by the time eval() returns -- and is refunded when the
// handle closes.
$roomy = new Sandbox(new SandboxConfig(
	capabilities: (new Capabilities())->with(vfs: true),
	filesystem: new MemoryFileSystem($files),
	limits: new Limits(memoryBytes: 8 * 1024 * 1024),
	vfsQuota: new VfsQuota(maxFileBytes: 0),
));

$roomy->registerLibrary('host', [
	'memory' => static function () use (&$roomy): int {
		return $roomy->stats()->memoryBytes;
	},
]);

[$before, $whileOpen, $afterClose] = $roomy->eval(<<<'LUA'
	local before = host.memory()

	local f = io.open("/small.txt", "r")
	local while_open = host.memory()

	f:close()
	local after_close = host.memory()

	return before, while_open, after_close
	LUA, '=observe-mid-call');

printf("while open  => %s\n",
	$whileOpen - $before >= 256 * 1024 ? 'buffer counted' : sprintf('MISSING (%d -> %d)', $before, $whileOpen));
printf("after close => %s\n",
	$whileOpen - $afterClose >= 200 * 1024 ? 'refunded' : sprintf('STUCK (%d -> %d)', $whileOpen, $afterClose));

// The ranged read-it-all path: maxFileBytes 0 means "no ceiling", and the
// request that reaches readRange() must say "everything", never -1.
$ranged = new Sandbox(new SandboxConfig(
	capabilities: (new Capabilities())->with(vfs: true),
	filesystem: new RangedMemoryFileSystem($files),
	vfsQuota: new VfsQuota(maxFileBytes: 0),
));

[$bytes] = $ranged->eval(<<<'LUA'
	local f = io.open("/tiny.txt", "r")
	local body = f:read("a")
	f:close()

	return #body
	LUA, '=read-all');

printf("read all   => %d bytes\n", $bytes);

$roomy->close();
$ranged->close();

?>
--EXPECT--
open big  => MemoryLimitError
while open  => buffer counted
after close => refunded
read all   => 100 bytes
