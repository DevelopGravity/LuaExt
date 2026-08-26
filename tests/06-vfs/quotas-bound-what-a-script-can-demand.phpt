--TEST--
Every VfsQuota field bounds something a script can actually reach for
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

require __DIR__ . '/memory-filesystem.inc';

use DevelopGravity\LuaExt\Capabilities;
use DevelopGravity\LuaExt\Exception\FatalError;
use DevelopGravity\LuaExt\Sandbox;
use DevelopGravity\LuaExt\SandboxConfig;
use DevelopGravity\LuaExt\VfsQuota;

// A quota that is validated at construction and never read afterwards is worse
// than no quota, because the host believes it has one. Each row below drives the
// script at exactly one field and shows the bound being reached.
//
// They split into two kinds, and the split is the interesting part:
//
//   RESOURCE BOUNDS -- open handles, total buffered bytes, file count, backend
//   operations -- are FATAL. Each one caps something the host has already spent
//   or must keep holding, so a script able to catch one would retry and the host
//   would pay again.
//
//   REQUEST VALIDATION -- a range past maxFileBytes, an overlong or too-deep
//   path -- is CATCHABLE. All three are refused before the backend is called at
//   all, so a refusal costs the host nothing and the script can legitimately
//   adapt: write less, use a shorter name. Catching one does not let a script
//   exceed the bound, only learn where it is.

$run = static function (VfsQuota $quota, string $script, array $files = []): string {
	$sandbox = new Sandbox(new SandboxConfig(
		capabilities: (new Capabilities())->with(vfs: true, vfsWrite: true),
		filesystem: new MemoryFileSystem($files),
		vfsQuota: $quota,
	));

	try {
		(void) $sandbox->eval($script, '=quota');
		$outcome = 'NOT ENFORCED';
	} catch (Throwable $error) {
		$outcome = sprintf(
			'[%s] %s',
			$error instanceof FatalError ? 'fatal' : 'catchable',
			$error->getMessage(),
		);
	}

	$sandbox->close();

	return $outcome;
};

printf("openHandles: %s\n", $run(
	new VfsQuota(maxOpenHandles: 2),
	'local kept = {} for i = 1, 8 do kept[i] = assert(io.open("/f" .. i .. ".txt", "w")) end',
));

printf("fileBytes:   %s\n", $run(
	new VfsQuota(maxFileBytes: 64),
	'local f = assert(io.open("/big.txt", "w")) f:write(string.rep("x", 4096))',
));

printf("totalBytes:  %s\n", $run(
	new VfsQuota(maxTotalBytes: 256),
	'local kept = {} for i = 1, 8 do
		local f = assert(io.open("/f" .. i .. ".txt", "w"))
		f:write(string.rep("x", 200))
		kept[i] = f
	end',
));

printf("files:       %s\n", $run(
	new VfsQuota(maxFiles: 3),
	'for i = 1, 20 do local f = assert(io.open("/n" .. i .. ".txt", "w")) f:close() end',
));

printf("operations:  %s\n", $run(
	new VfsQuota(maxOperations: 12),
	'for i = 1, 50 do local f = io.open("/probe.txt", "r") if f then f:close() end end',
));

// Path length and depth are refused before the backend is ever asked, which is
// why they are checked against a sandbox with no files at all.
printf("pathLength:  %s\n", $run(
	new VfsQuota(maxPathLength: 16),
	'io.open("/" .. string.rep("a", 200) .. ".txt", "r")',
));

printf("pathDepth:   %s\n", $run(
	new VfsQuota(maxPathDepth: 2),
	'io.open("/a/b/c/d/e/f.txt", "r")',
));

// Deleting gives the budget back, or maxFiles would describe files ever created
// rather than files that exist.
$sandbox = new Sandbox(new SandboxConfig(
	capabilities: (new Capabilities())->with(vfs: true, vfsWrite: true),
	filesystem: new MemoryFileSystem(),
	vfsQuota: new VfsQuota(maxFiles: 2),
));

var_dump($sandbox->eval('
	local made = 0
	for round = 1, 6 do
		local f = assert(io.open("/reused.txt", "w"))
		f:write("x")
		f:close()
		made = made + 1
		assert(os.remove("/reused.txt"))
	end
	return made
', '=reuse')[0]);

$sandbox->close();

?>
--EXPECTF--
openHandles: [fatal] The sandbox already has 2 file(s) open, which is its VfsQuota::$maxOpenHandles
fileBytes:   [catchable] A file range ending at byte %d exceeds the 64 byte VfsQuota::$maxFileBytes
totalBytes:  [fatal] Buffering %d more byte(s) would pass the 256 byte VfsQuota::$maxTotalBytes
files:       [fatal] The filesystem already holds 3 file(s), which is its VfsQuota::$maxFiles
operations:  [fatal] This call has already made 12 filesystem operation(s), which is its VfsQuota::$maxOperations
pathLength:  [catchable] This path cannot be used: %s
pathDepth:   [catchable] This path cannot be used: %s
int(6)
