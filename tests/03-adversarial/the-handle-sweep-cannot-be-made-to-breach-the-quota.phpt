--TEST--
A script cannot spend the operation quota and leave the sweep's flush to breach it
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

require __DIR__ . '/../06-vfs/memory-filesystem.inc';

use DevelopGravity\LuaExt\Capabilities;
use DevelopGravity\LuaExt\Sandbox;
use DevelopGravity\LuaExt\SandboxConfig;
use DevelopGravity\LuaExt\VfsQuota;

// The call-scope sweep closes whatever a script left open, and closing a dirty
// handle writes its buffer to the backend. The sweep runs after the protected
// call has already returned -- a raise there has no handler and takes the whole
// request down through lua_atpanic. So the flush must not be a charged
// operation: a script that spends its VfsQuota::$maxOperations budget down to
// nothing and returns with a dirty handle would otherwise make the sweep's own
// write the breaching one.
//
// maxOperations: 1, and opening a new file for writing costs exactly that one
// operation (the existence probe). The write lands in the handle's buffer for
// free, the script returns without closing, and the sweep's flush is the next
// backend call -- charged, it would be operation two of one.

$backend = new MemoryFileSystem();

$sandbox = new Sandbox(new SandboxConfig(
	capabilities: (new Capabilities())->with(vfs: true, vfsWrite: true),
	filesystem: $backend,
	vfsQuota: new VfsQuota(maxOperations: 1),
));

[$written] = $sandbox->eval(<<<'LUA'
	local f = io.open("/left-open.txt", "w")
	f:write("flushed by the sweep")

	-- Returned deliberately unclosed: the sweep owns it now.
	return true
	LUA, '=spend-then-abandon');

printf("script   => %s\n", $written ? 'returned normally' : 'NO');

// The flush is still visible in the stats: uncharged is about the quota, not
// the bookkeeping. One operation for the open's existence probe, one for the
// sweep's write.
printf("counted  => %s\n", $sandbox->stats()->vfsOperations >= 2 ? 'yes' : 'NO');

$sandbox->close();

// The sweep really flushed to the HOST, not to sandbox state: a second sandbox
// with a budget of its own reads the bytes back off the same backend object.
$reader = new Sandbox(new SandboxConfig(
	capabilities: (new Capabilities())->with(vfs: true),
	filesystem: $backend,
	vfsQuota: new VfsQuota(maxOperations: 100),
));

[$contents] = $reader->eval(<<<'LUA'
	local f = io.open("/left-open.txt", "r")
	local body = f:read("a")
	f:close()

	return body
	LUA, '=read-back');

printf("flushed  => %s\n", $contents);
$reader->close();

?>
--EXPECT--
script   => returned normally
counted  => yes
flushed  => flushed by the sweep
