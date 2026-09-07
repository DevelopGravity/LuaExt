--TEST--
VfsQuota::$billWallTime decides whether backend time counts against the wall-clock deadline
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

require __DIR__ . '/../06-vfs/memory-filesystem.inc';

use DevelopGravity\LuaExt\Capabilities;
use DevelopGravity\LuaExt\Exception\WallClockLimitError;
use DevelopGravity\LuaExt\FileSystem;
use DevelopGravity\LuaExt\Limits;
use DevelopGravity\LuaExt\Sandbox;
use DevelopGravity\LuaExt\SandboxConfig;
use DevelopGravity\LuaExt\VfsQuota;

// Everything else about this toggle was only ever exercised as plumbing --
// constructed, copied, reflected. This is the behavioral pair: with the bill
// ON a backend nap longer than the wall budget breaches it (surfacing only
// AFTER the backend call returns, since the interrupt lands when Lua
// resumes); with it OFF the same nap is forgiven by the deadline but still
// MEASURED, which is what separates "not billed" from "not observed".

final class NappingFileSystem implements FileSystem
{
	use MemoryFileSystemBody {
		read as private innerRead;
	}

	public int $napMicroseconds = 0;
	public int $readsCompleted = 0;

	public function read(string $path): string
	{
		usleep($this->napMicroseconds);
		$contents = $this->innerRead($path);
		$this->readsCompleted++;

		return $contents;
	}
}

$build = static function (bool $billWallTime): array {
	$filesystem = new NappingFileSystem(['/f.txt' => 'payload']);
	$filesystem->napMicroseconds = 250000;

	$sandbox = new Sandbox(new SandboxConfig(
		capabilities: (new Capabilities())->with(vfs: true),
		limits: (new Limits())->with(cpuSeconds: null, wallClockSeconds: 0.1),
		filesystem: $filesystem,
		vfsQuota: new VfsQuota(billWallTime: $billWallTime),
	));

	return [$sandbox, $filesystem];
};

$script = <<<'LUA'
	local f = io.open("/f.txt", "r")
	local data = f:read("a")
	local n = 0
	for i = 1, 1000000 do n = n + 1 end
	return data
LUA;

// Billed: the 0.25s nap spends the 0.1s wall budget.
[$sandbox, $filesystem] = $build(true);

try {
	(void) $sandbox->eval($script, '=billed');
	print "billed:   RETURNED\n";
} catch (WallClockLimitError $error) {
	printf("billed:   WallClockLimitError, after the read completed: %s\n",
		var_export($filesystem->readsCompleted === 1, true));
}
$sandbox->close();

// Unbilled: the same nap is forgiven by the deadline, yet still measured.
[$sandbox, $filesystem] = $build(false);

[$data] = $sandbox->eval($script, '=unbilled');
$stats = $sandbox->stats();

printf("unbilled: returned %s\n", $data);
printf("unbilled: wall stayed under the limit: %s\n",
	var_export($stats->wallClockSeconds < 0.1, true));
printf("unbilled: the nap was still measured:  %s\n",
	var_export($stats->vfsWallClockSeconds >= 0.2, true));

$sandbox->close();

?>
--EXPECT--
billed:   WallClockLimitError, after the read completed: true
unbilled: returned payload
unbilled: wall stayed under the limit: true
unbilled: the nap was still measured:  true
