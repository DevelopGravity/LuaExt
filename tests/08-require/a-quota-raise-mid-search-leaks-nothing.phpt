--TEST--
A quota raise in the middle of require()'s VFS search leaks nothing and leaves the sandbox usable
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

require __DIR__ . '/../06-vfs/memory-filesystem.inc';

use DevelopGravity\LuaExt\Capabilities;
use DevelopGravity\LuaExt\Exception\FatalError;
use DevelopGravity\LuaExt\Sandbox;
use DevelopGravity\LuaExt\SandboxConfig;
use DevelopGravity\LuaExt\VfsQuota;

// The module search asks the backend "exists?" and then "read", and each of
// those charges VfsQuota::$maxOperations. A budget of one lets the first
// charge through and raises on the second -- a longjmp out of the middle of
// the search loop, at the exact moment the loop is holding the canonical
// path it built for the backend. That path used to be owned by the C frame
// the raise unwound past, so every quota refusal mid-search leaked it; it is
// now anchored in Lua-owned memory, which the unwind hands to the collector.
// A debug-build run of this test is what proves the leak stays gone.
$make = static function (int $operations): Sandbox {
	return new Sandbox(new SandboxConfig(
		capabilities: (new Capabilities())->with(require: true, vfs: true),
		filesystem: new MemoryFileSystem(['/mod.lua' => 'return { answer = 42 }']),
		vfsQuota: new VfsQuota(maxOperations: $operations),
	));
};

$sandbox = $make(1);

try {
	(void) $sandbox->eval('require("mod")', '=starved');
	echo "NOT REFUSED\n";
} catch (Throwable $error) {
	printf(
		"[%s] %s\n",
		$error instanceof FatalError ? 'fatal' : 'catchable',
		$error->getMessage(),
	);
}

$sandbox->close();

// Two operations is exactly what the search costs -- one exists, one read --
// so a budget of two lets the very same lookup run end to end. That the
// refusal above happened between those two charges is what put the raise in
// the middle of the loop.
$sandbox = $make(2);

[$loaded] = $sandbox->eval('return require("mod").answer', '=budgeted');
var_dump($loaded);

$sandbox->close();

?>
--EXPECT--
[fatal] This call has already made 1 filesystem operation(s), which is its VfsQuota::$maxOperations
int(42)
