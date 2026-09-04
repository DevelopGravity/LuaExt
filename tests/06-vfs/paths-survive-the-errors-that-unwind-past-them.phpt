--TEST--
A canonical path outlives the unwind that abandons it
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

// Every entry point that takes a path canonicalises it into a zend_string and
// then calls the backend, and every one of those calls can raise rather than
// return: a spent quota, a FileSystem that is gone, a method the interface
// promises. luaext_error_raise() ends in lua_error(), which longjmps, and a
// longjmp runs no cleanup at all.
//
// So each row here drives one entry point into a raise while a path is live.
// What they assert is not the message -- that is only how the case is named --
// but two things the message cannot say for itself:
//
//   NOTHING LEAKED. A debug PHP reports what the request never freed, and the
//   io.open rows below reported three leaks before the path became Lua's to own.
//   A release build reports nothing here, which is exactly why this file exists:
//   it is the debug and sanitizer legs that read it.
//
//   NOTHING WAS READ AFTER IT WAS FREED. io.lines released the path and then
//   formatted it into the very error it was raising. The path in that message is
//   long and distinctive so a stale read shows up as wrong text rather than as
//   the right text still lying around in freed memory.

$run = static function (VfsQuota $quota, string $script, array $files = []): string {
	$sandbox = new Sandbox(new SandboxConfig(
		capabilities: (new Capabilities())->with(vfs: true, vfsWrite: true),
		filesystem: new MemoryFileSystem($files),
		vfsQuota: $quota,
	));

	try {
		(void) $sandbox->eval($script, '=unwind');
		$outcome = 'NOT RAISED';
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

// io.lines is the use-after-free: it released the path, then read it back to
// build this message. The name is long enough that a freed buffer would have to
// survive intact to pass by accident.
printf("io.lines:   %s\n", $run(
	new VfsQuota(),
	'for line in io.lines("/a-deliberately-long-name-to-read-back.txt") do end',
));

// The quota is spent by the loop, so the raise lands inside os.remove with its
// path canonicalised and live.
printf("os.remove:  %s\n", $run(
	new VfsQuota(maxOperations: 4),
	'for i = 1, 50 do os.remove("/gone" .. i .. ".txt") end',
));

// os.rename holds two at once, and the second is canonicalised while the first
// is already live.
printf("os.rename:  %s\n", $run(
	new VfsQuota(maxOperations: 4),
	'for i = 1, 50 do os.rename("/from" .. i .. ".txt", "/to" .. i .. ".txt") end',
));

// io.open, the case that was found first: the handle cap raises from inside the
// open, with the path still held by the caller.
printf("io.open:    %s\n", $run(
	new VfsQuota(maxOpenHandles: 2),
	'local kept = {} for i = 1, 8 do kept[i] = assert(io.open("/held" .. i .. ".txt", "w")) end',
));

// A backend that throws for real, so the raise comes from the exception
// boundary rather than from a quota -- a different unwind past the same path.
$sandbox = new Sandbox(new SandboxConfig(
	capabilities: (new Capabilities())->with(vfs: true, vfsWrite: true),
	filesystem: $backend = new MemoryFileSystem(),
));

$backend->failOn = ['exists'];

try {
	(void) $sandbox->eval('io.open("/host-is-broken.txt", "r")', '=hostfail');
	echo "host failure: NOT RAISED\n";
} catch (Throwable $error) {
	printf("host failure: %s: %s\n", $error::class, $error->getMessage());
}

$sandbox->close();

?>
--EXPECT--
io.lines:   [catchable] Cannot open '/a-deliberately-long-name-to-read-back.txt' for reading
os.remove:  [fatal] This call has already made 4 filesystem operation(s), which is its VfsQuota::$maxOperations
os.rename:  [fatal] This call has already made 4 filesystem operation(s), which is its VfsQuota::$maxOperations
io.open:    [fatal] The sandbox already has 2 file(s) open, which is its VfsQuota::$maxOpenHandles
host failure: RuntimeException: backend exploded during exists
