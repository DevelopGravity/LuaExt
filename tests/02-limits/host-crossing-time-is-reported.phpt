--TEST--
SandboxStats reports wall and CPU time spent inside host crossings
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

// billHostTime decides whether host time is BILLED against the limits; these
// four stats measure it either way. A host that leaves billing off still gets
// to see what its callbacks and filesystem cost -- which is the only way to
// notice a slow backend without turning it into the script's problem.
//
// The buckets are disjoint and mirror the counters: phpWallClockSeconds /
// phpCpuSeconds cover calls out to PHP, vfsWallClockSeconds / vfsCpuSeconds
// cover FileSystem backend calls.

const NAP_SECONDS = 0.05;

// A sandbox that never crosses reports exact zeros -- not merely small values.
$idle = new Sandbox(new SandboxConfig());
(void) $idle->eval('local x = 0 for i = 1, 1000 do x = x + i end return x', '=idle');
$stats = $idle->stats();
printf(
	"idle     php %.1f/%.1f vfs %.1f/%.1f\n",
	$stats->phpWallClockSeconds,
	$stats->phpCpuSeconds,
	$stats->vfsWallClockSeconds,
	$stats->vfsCpuSeconds
);
$idle->close();

// A sleeping callable is wall time without CPU time: the nap must land in the
// php wall bucket, leave its CPU bucket close to nothing, and leave the vfs
// buckets untouched. Unbilled by default, so wallClockSeconds stays near zero
// while phpWallClockSeconds does not -- the measurement is not the billing.
$sleeper = new Sandbox(new SandboxConfig());
$sleeper->registerLibrary('host', ['nap' => static function (): int {
	usleep((int) (NAP_SECONDS * 1_000_000));

	return 1;
}]);
(void) $sleeper->eval('return host.nap()', '=nap');
$stats = $sleeper->stats();
printf(
	"callable php wall %s, php cpu %s, vfs wall %s, billed wall %s\n",
	$stats->phpWallClockSeconds >= NAP_SECONDS * 0.8 ? 'counted' : sprintf('lost (%.4f)', $stats->phpWallClockSeconds),
	$stats->phpCpuSeconds < NAP_SECONDS ? 'small' : sprintf('inflated (%.4f)', $stats->phpCpuSeconds),
	$stats->vfsWallClockSeconds === 0.0 ? 'zero' : sprintf('leaked (%.4f)', $stats->vfsWallClockSeconds),
	$stats->wallClockSeconds < NAP_SECONDS ? 'small' : sprintf('billed (%.4f)', $stats->wallClockSeconds)
);

// A second call accumulates rather than replaces.
$before = $stats->phpWallClockSeconds;
(void) $sleeper->eval('return host.nap()', '=nap-again');
printf(
	"again    php wall %s\n",
	$sleeper->stats()->phpWallClockSeconds >= $before + NAP_SECONDS * 0.8 ? 'accumulated' : 'reset'
);
$sleeper->close();

// A busy callable is CPU time: spinning the host thread must move the php cpu
// bucket. The threshold is loose on purpose -- Windows measures thread CPU in
// ~15.6ms scheduler ticks, so 60ms of spinning is only promised to show up at
// all, not to show up precisely.
$burner = new Sandbox(new SandboxConfig());
$burner->registerLibrary('host', ['burn' => static function (): int {
	$until = hrtime(true) + 60_000_000;
	$spin = 0;

	while (hrtime(true) < $until) {
		$spin++;
	}

	return $spin;
}]);
(void) $burner->eval('return host.burn()', '=burn');
$stats = $burner->stats();
printf(
	"burn     php cpu %s\n",
	$stats->phpCpuSeconds > 0.0 ? 'counted' : 'lost'
);
$burner->close();

// A slow FileSystem lands in the vfs buckets and only there: reading a file
// through io.open must move vfs wall time while the php buckets stay zero,
// because no registered callable, output callback or resolver ever ran.
$slowFs = new class implements FileSystem {
	public function exists(string $path): bool
	{
		usleep((int) (NAP_SECONDS * 1_000_000));

		return true;
	}

	public function read(string $path): string
	{
		usleep((int) (NAP_SECONDS * 1_000_000));

		return "payload\n";
	}

	public function write(string $path, string $contents): void
	{
	}

	public function delete(string $path): void
	{
	}

	public function rename(string $from, string $to): void
	{
	}

	public function stat(string $path): ?FileStat
	{
		return new FileStat(size: 8, mtime: 0, isDirectory: false);
	}

	public function list(string $path): array
	{
		return [];
	}
};

$reader = new Sandbox(new SandboxConfig(
	capabilities: (new Capabilities())->with(vfs: true),
	filesystem: $slowFs,
));
(void) $reader->eval(
	'local f = io.open("data.txt", "r") local body = f:read("a") f:close() return #body',
	'=vfs'
);
$stats = $reader->stats();
printf(
	"vfs      wall %s, php wall %s\n",
	$stats->vfsWallClockSeconds >= NAP_SECONDS * 0.8 ? 'counted' : sprintf('lost (%.4f)', $stats->vfsWallClockSeconds),
	$stats->phpWallClockSeconds === 0.0 ? 'zero' : sprintf('leaked (%.4f)', $stats->phpWallClockSeconds)
);
$reader->close();

?>
--EXPECT--
idle     php 0.0/0.0 vfs 0.0/0.0
callable php wall counted, php cpu small, vfs wall zero, billed wall small
again    php wall accumulated
burn     php cpu counted
vfs      wall counted, php wall zero
