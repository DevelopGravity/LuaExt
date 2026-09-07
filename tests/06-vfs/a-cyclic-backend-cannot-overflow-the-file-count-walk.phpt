--TEST--
The maxFiles count walk survives a backend reporting a bottomless tree, even with maxPathDepth 0
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Capabilities;
use DevelopGravity\LuaExt\FileStat;
use DevelopGravity\LuaExt\FileSystem;
use DevelopGravity\LuaExt\Sandbox;
use DevelopGravity\LuaExt\SandboxConfig;
use DevelopGravity\LuaExt\VfsQuota;

// The count walk recurses on the C stack, one frame per directory level, and
// the only configurable bound on it is VfsQuota::$maxPathDepth -- whose zero
// means "no bound from the quota" everywhere in this extension. A backend is
// host code, not trusted code: one that answers every list() with another
// directory describes a bottomless tree, and with the quota bound switched
// off only the walk's own fixed ceiling stands between that answer and C
// stack exhaustion. This backend is exactly that answer.
final class BottomlessFileSystem implements FileSystem
{
	public function exists(string $path): bool
	{
		return false;
	}

	public function stat(string $path): ?FileStat
	{
		// Everything is a directory, so every child recurses.
		return new FileStat(0, 0, isDirectory: true);
	}

	public function read(string $path): string
	{
		return '';
	}

	public function write(string $path, string $contents): void
	{
	}

	public function delete(string $path): void
	{
	}

	public function rename(string $from, string $to): void
	{
	}

	public function list(string $path): array
	{
		// One more level, forever. A '..' entry would be the same trap by a
		// shorter route; the walk skips those outright.
		return ['deeper', '..', '.', '', 'not/a/child'];
	}
}

$sandbox = new Sandbox(new SandboxConfig(
	capabilities: (new Capabilities())->with(vfs: true, vfsWrite: true),
	filesystem: new BottomlessFileSystem(),
	vfsQuota: new VfsQuota(maxFiles: 4, maxPathDepth: 0, maxOperations: 0),
));

// Creating the first file is what triggers the count. Reaching the ceiling
// just stops the walk -- everything it saw was a directory, so the count is
// zero, the quota is satisfied, and the write goes through. The assertion is
// that this line RETURNS instead of taking the process down.
[$outcome] = $sandbox->eval('
	local handle = assert(io.open("/created.txt", "w"))
	handle:write("x")
	handle:close()
	return "survived the walk"
', '=bottomless');

var_dump($outcome);

$sandbox->close();

?>
--EXPECT--
string(17) "survived the walk"
