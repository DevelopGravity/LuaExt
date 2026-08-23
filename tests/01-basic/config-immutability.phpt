--TEST--
Configuration objects reject writes, re-construction, cloning and new properties
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Capabilities;
use DevelopGravity\LuaExt\Exception\ConfigurationError;
use DevelopGravity\LuaExt\FileStat;
use DevelopGravity\LuaExt\Limits;
use DevelopGravity\LuaExt\ModuleSource;
use DevelopGravity\LuaExt\SandboxConfig;
use DevelopGravity\LuaExt\SandboxStats;
use DevelopGravity\LuaExt\VfsQuota;

function show(callable $attempt): void
{
	try {
		$attempt();
		echo "NOT REFUSED\n";
	} catch (Throwable $error) {
		printf("%s: %s\n", $error::class, $error->getMessage());
	}
}

$capabilities = new Capabilities();
$limits = new Limits();

// readonly: a widening has to go through with(), where it is visible.
show(static function () use ($capabilities): void {
	$capabilities->vfs = true;
});
show(static function () use ($limits): void {
	$limits->cpuSeconds = null;
});

// @strict-properties: a misspelled field is not quietly added on the side.
show(static function () use ($capabilities): void {
	$capabilities->vfsWrites = true;
});

echo "--- reconstruction ---\n";

// An internal constructor writes property slots directly, so the engine's
// readonly guard never sees them. Calling it again would overwrite committed
// values and leak what they pointed at.
show(static function () use ($capabilities): void {
	$capabilities->__construct(vfs: true);
});
show(static function () use ($limits): void {
	$limits->__construct();
});

var_dump($capabilities->vfs, $limits->cpuSeconds);

echo "--- cloning ---\n";

// These objects are immutable and carry no identity, so a clone would be
// indistinguishable from what it was cloned from. with() is the derivation
// path, and refusing here is what keeps it the only one.
foreach ([
	new Capabilities(),
	new Limits(),
	new VfsQuota(),
	new SandboxConfig(),
	new FileStat(0, 0),
	new ModuleSource('', '=(test)'),
] as $object) {
	show(static function () use ($object): void {
		$copy = clone $object;
	});
}

$stats = (new ReflectionClass(SandboxStats::class))->newInstanceWithoutConstructor();
show(static function () use ($stats): void {
	$copy = clone $stats;
});

?>
--EXPECT--
Error: Cannot modify readonly property DevelopGravity\LuaExt\Capabilities::$vfs
Error: Cannot modify readonly property DevelopGravity\LuaExt\Limits::$cpuSeconds
Error: Cannot create dynamic property DevelopGravity\LuaExt\Capabilities::$vfsWrites
--- reconstruction ---
DevelopGravity\LuaExt\Exception\ConfigurationError: DevelopGravity\LuaExt\Capabilities is immutable and has already been constructed. Build a separate object, or derive one with with().
DevelopGravity\LuaExt\Exception\ConfigurationError: DevelopGravity\LuaExt\Limits is immutable and has already been constructed. Build a separate object, or derive one with with().
bool(false)
float(1)
--- cloning ---
Error: Trying to clone an uncloneable object of class DevelopGravity\LuaExt\Capabilities
Error: Trying to clone an uncloneable object of class DevelopGravity\LuaExt\Limits
Error: Trying to clone an uncloneable object of class DevelopGravity\LuaExt\VfsQuota
Error: Trying to clone an uncloneable object of class DevelopGravity\LuaExt\SandboxConfig
Error: Trying to clone an uncloneable object of class DevelopGravity\LuaExt\FileStat
Error: Trying to clone an uncloneable object of class DevelopGravity\LuaExt\ModuleSource
Error: Trying to clone an uncloneable object of class DevelopGravity\LuaExt\SandboxStats
