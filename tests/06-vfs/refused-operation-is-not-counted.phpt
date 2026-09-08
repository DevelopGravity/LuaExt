--TEST--
The operation that trips VfsQuota::$maxOperations never reached the backend, and is not counted
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

$sandbox = new Sandbox(new SandboxConfig(
	capabilities: (new Capabilities())->with(vfs: true),
	filesystem: new MemoryFileSystem(['/a.txt' => 'hello']),
	vfsQuota: new VfsQuota(maxOperations: 5),
));

// Drive well past the cap in one call. The attempt that trips the quota is
// refused BEFORE the backend is called, so the published counter must show
// exactly the cap -- counting the refusal would claim a backend crossing that
// never happened.
try {
	(void) $sandbox->eval(
		'for i = 1, 20 do local f = assert(io.open("/a.txt", "r")) f:read("a") f:close() end',
		'=ops',
	);
	echo "NOT ENFORCED\n";
} catch (FatalError) {
	echo "quota tripped\n";
}

var_dump($sandbox->stats()->vfsOperations);

$sandbox->close();

?>
--EXPECT--
quota tripped
int(5)
