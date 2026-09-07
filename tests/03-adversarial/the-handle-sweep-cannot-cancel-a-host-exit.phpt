--TEST--
A dirty VFS handle at the boundary does not cancel a host's exit()
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

// host-exit-is-not-cancelled-by-the-sandbox.phpt pins the plain path. This one
// arms the trap that used to re-break it: the end-of-call handle sweep flushes
// dirty handles and clears whatever exception the flush raised -- and it used
// to clear an exception that was ALREADY PENDING when it started, including
// exit()'s unwind sentinel. A host that exits with unflushed writes must still
// exit; the flush is forfeited, not the exit.

$inc = realpath(__DIR__ . '/../06-vfs/memory-filesystem.inc');

$script = sprintf(<<<'PHP'
	require %s;

	use DevelopGravity\LuaExt\Capabilities;
	use DevelopGravity\LuaExt\Sandbox;
	use DevelopGravity\LuaExt\SandboxConfig;

	$sandbox = new Sandbox(new SandboxConfig(
		capabilities: (new Capabilities())->with(vfs: true, vfsWrite: true),
		filesystem: new MemoryFileSystem([]),
	));
	$sandbox->registerLibrary('host', [
		'stop' => static function (): void {
			exit(3);
		},
	]);

	echo "before\n";
	(void) $sandbox->eval(
		'local f = io.open("/dirty.txt", "w") f:write("unflushed") host.stop()',
		'=exiting',
	);
	echo "AFTER -- the exit was cancelled\n";
	PHP, var_export($inc, true));

$file = tempnam(sys_get_temp_dir(), 'luaext-sweep-exit-') . '.php';
file_put_contents($file, "<?php\n" . $script);

// The bare name, so PHP applies the platform prefix and suffix itself; see
// host-exit-is-not-cancelled-by-the-sandbox.phpt.
$command = sprintf(
	'%s -n -d extension_dir=%s -d extension=luaext %s 2>&1',
	escapeshellarg(PHP_BINARY),
	escapeshellarg(ini_get('extension_dir')),
	escapeshellarg($file),
);

exec($command, $output, $status);
unlink($file);

printf("output: %s\n", implode(' | ', $output));
printf("status: %d\n", $status);

?>
--EXPECT--
output: before
status: 3
