--TEST--
VfsQuota::$maxOperations is spent per call and refilled for the next one
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

// "Calls into the host backend PER SANDBOX CALL", says the stub, and the header
// spells out why: a host that runs many calls should not find the hundredth
// refused because the first ninety-nine spent the budget.
//
// That is what happened. luaext_vfs_begin_call() -- the function whose entire
// job is resetting the counter -- was written, declared, documented, and called
// from nowhere at all, so the counter only ever climbed. The quota was a
// per-sandbox-LIFETIME bound wearing a per-call name, and a long-lived sandbox
// eventually refused every filesystem call it was asked for.
//
// Nothing caught it because no test made enough calls to reach the default of
// 10000. This one does not need to: it sets the quota low and shows the SHAPE,
// which is the property that was wrong.

$sandbox = new Sandbox(new SandboxConfig(
	capabilities: (new Capabilities())->with(vfs: true, vfsWrite: true),
	filesystem: new MemoryFileSystem(),
	vfsQuota: new VfsQuota(maxOperations: 8),
));

// Four operations per call, comfortably inside a budget of eight -- so every
// one of these must succeed, however many there are.
$chunk = $sandbox->compile('
	local handle = assert(io.open("/counted.txt", "w"))
	handle:write("x")
	handle:close()
	return true
', '=per-call');

$survived = 0;

try {
	for ($index = 0; $index < 25; $index++) {
		(void) $chunk->call();
		$survived++;
	}
} catch (Throwable $error) {
	printf("stopped after %d calls: %s\n", $survived, $error->getMessage());
}

printf("calls that ran: %d\n", $survived);

// The budget is still a real bound WITHIN one call, which is the half that was
// never broken and must not be lost while fixing the other half.
try {
	(void) $sandbox->eval('
		for index = 1, 50 do
			local handle = assert(io.open("/inner" .. index .. ".txt", "w"))
			handle:close()
		end
	', '=within-one-call');
	echo "within one call: NOT REFUSED\n";
} catch (Throwable $error) {
	printf(
		"within one call: [%s] %s\n",
		$error instanceof FatalError ? 'fatal' : 'catchable',
		$error->getMessage(),
	);
}

// And the call after that refusal starts clean again.
var_dump($chunk->call());

$sandbox->close();

?>
--EXPECT--
calls that ran: 25
within one call: [fatal] This call has already made 8 filesystem operation(s), which is its VfsQuota::$maxOperations
array(1) {
  [0]=>
  bool(true)
}
