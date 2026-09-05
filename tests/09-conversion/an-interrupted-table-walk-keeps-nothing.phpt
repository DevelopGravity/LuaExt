--TEST--
A table conversion stopped mid-walk by an interrupt releases what it had built
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

// luaext_convert_table() walks a Lua table asking, not raising -- it holds a
// half-built PHP array and two Lua stack slots, so a longjmp would strand them.
// Its own comment says the interrupt check "unwinds through the ordinary
// failure path instead, which releases both", and every other failure in that
// function does exactly that by way of the `failed:` label.
//
// The interrupt check did not. It returned directly, handing its caller a live
// half-built array on a `false` return -- and both callers are written to the
// contract that a false return has nothing left to release. The inner array
// was dropped on the floor.
//
// Reproducing it needs the flag PENDING but NOT YET DELIVERED when the walk
// starts, which sounds like a race with the watchdog thread and is not:
//
//   1. Sandbox::interrupt() sets the sticky flag synchronously, and a VFS
//      backend is PHP, so a backend method can set it mid-script.
//   2. Nothing on the VFS return path delivers it.
//   3. LUAEXT_VMCHECK is patched into loop back edges and OP_TAILCALL only --
//      so a plain OP_CALL immediately after reaches argument conversion with
//      the flag still merely pending.
//
// The table is nested because the leak is the INNER array: the outer one is
// released by the caller either way, and only the inner is orphaned.
//
// Verified to catch it: against the unfixed build this reports ~401 KB of
// growth over 20 runs, ~20 KB per run, which is the 1022 entries the inner
// walk had converted when the check fired.

final class TrippingFileSystem implements FileSystem
{
	public ?Sandbox $sandbox = null;

	public function exists(string $path): bool
	{
		$this->sandbox?->interrupt();

		return false;
	}

	public function stat(string $path): ?FileStat { return null; }
	public function read(string $path): string { return ''; }
	public function write(string $path, string $contents): void {}
	public function delete(string $path): void {}
	public function rename(string $from, string $to): void {}
	public function list(string $path): array { return []; }
}

const SCRIPT = <<<'LUA'
	local inner = {}
	for i = 1, 4000 do inner[i] = i end
	local outer = { inner = inner }

	io.open("/trip", "r")

	local answer = host.consume(outer)

	return answer
LUA;

$run = static function (): string {
	$filesystem = new TrippingFileSystem();

	$sandbox = new Sandbox(new SandboxConfig(
		capabilities: (new Capabilities())->with(vfs: true),
		filesystem: $filesystem,
	));

	$filesystem->sandbox = $sandbox;
	$sandbox->registerLibrary('host', ['consume' => static fn (mixed $table): int => 1]);

	try {
		(void) $sandbox->eval(SCRIPT, '=trip');
		$outcome = 'NOT INTERRUPTED';
	} catch (Throwable $error) {
		$outcome = $error::class;
	}

	$sandbox->close();

	return $outcome;
};

// One warm-up, so first-touch allocation is not counted as growth.
$outcome = $run();

$before = memory_get_usage();

for ($i = 0; $i < 20; $i++) {
	$run();
}

$growth = memory_get_usage() - $before;

printf("interrupt delivered: %s\n", $outcome);

// Generous: the leak is ~20 KB per run and this allows 50 KB across all 20.
printf("retains nothing across 20 runs: %s\n", $growth < 51200 ? 'yes' : "no ({$growth} bytes)");

?>
--EXPECT--
interrupt delivered: DevelopGravity\LuaExt\Exception\HostAbortError
retains nothing across 20 runs: yes
