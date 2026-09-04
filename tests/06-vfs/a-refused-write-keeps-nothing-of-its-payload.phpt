--TEST--
A write refused mid-flight releases the payload it had already copied
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

// A RANGED backend routes io.write straight through to writeRange(), and the
// argument list it builds owns a COPY of the bytes -- ZVAL_STRINGL, not a
// borrowed ZVAL_STR, because the script's string is Lua's and the backend may
// keep what it is handed.
//
// That copy is live across the call, and the call raises when the operations
// quota runs out. A raise longjmps, so the dtor on the next line never ran and
// every refused write leaked its whole payload. The same shape as the path leak
// in luaext_vfs.c, one file over, and with a script-controlled size: a loop
// writing 1 MB chunks leaked a megabyte per refusal.
//
// A release build reports nothing here. This test earns its place on the debug
// and sanitizer legs, which is where the leak is visible at all.

$sandbox = new Sandbox(new SandboxConfig(
	capabilities: (new Capabilities())->with(vfs: true, vfsWrite: true),
	filesystem: new RangedMemoryFileSystem(),
	vfsQuota: new VfsQuota(maxOperations: 6),
));

try {
	(void) $sandbox->eval('
		local handle = assert(io.open("/payload.txt", "w"))
		for _ = 1, 50 do
			handle:write(string.rep("payload", 1024))
		end
	', '=refused-write');
	echo "NOT REFUSED\n";
} catch (Throwable $error) {
	printf(
		"[%s] %s\n",
		$error instanceof FatalError ? 'fatal' : 'catchable',
		$error->getMessage(),
	);
}

$sandbox->close();

// The buffered backend reaches the same quota by a different route -- read()
// and write() rather than writeRange() -- so both argument-building paths are
// exercised rather than only the one that was wrong.
$buffered = new Sandbox(new SandboxConfig(
	capabilities: (new Capabilities())->with(vfs: true, vfsWrite: true),
	filesystem: new MemoryFileSystem(),
	vfsQuota: new VfsQuota(maxOperations: 6),
));

try {
	(void) $buffered->eval('
		for index = 1, 50 do
			local handle = assert(io.open("/buffered" .. index .. ".txt", "w"))
			handle:write(string.rep("payload", 1024))
			handle:close()
		end
	', '=refused-buffered');
	echo "NOT REFUSED\n";
} catch (Throwable $error) {
	printf(
		"[%s] %s\n",
		$error instanceof FatalError ? 'fatal' : 'catchable',
		$error->getMessage(),
	);
}

$buffered->close();

?>
--EXPECT--
[fatal] This call has already made 6 filesystem operation(s), which is its VfsQuota::$maxOperations
[fatal] This call has already made 6 filesystem operation(s), which is its VfsQuota::$maxOperations
