--TEST--
Limits, VfsQuota and SandboxConfig construct with exactly their documented defaults
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Limits;
use DevelopGravity\LuaExt\OutputMode;
use DevelopGravity\LuaExt\OverflowBehavior;
use DevelopGravity\LuaExt\SandboxConfig;
use DevelopGravity\LuaExt\VfsQuota;

/**
 * Prints every declared property, so a field added to the stub without being
 * populated by the constructor shows up here as an uninitialised-property Error
 * rather than as a crash somewhere much later.
 */
function dumpValueObject(object $object): void
{
	printf("%s\n", $object::class);

	foreach ((new ReflectionObject($object))->getProperties() as $property) {
		$value = $property->getValue($object);

		printf("  %-20s %s\n", $property->getName(), match (true) {
			$value instanceof UnitEnum => $value::class . '::' . $value->name,
			is_array($value) => '[' . implode(', ', $value) . ']',
			default => var_export($value, true),
		});
	}
}

dumpValueObject(new Limits());
dumpValueObject(new VfsQuota());
dumpValueObject(new SandboxConfig());

// A null capabilities/limits/vfsQuota is not "missing", it is "the defaults":
// the C side resolves it to the same policy an explicit object would give.
var_dump((new SandboxConfig())->capabilities, (new SandboxConfig())->limits);

var_dump((new Limits())->outputOverflow === OverflowBehavior::Fail);
var_dump((new SandboxConfig())->outputMode === OutputMode::Buffer);

?>
--EXPECT--
DevelopGravity\LuaExt\Limits
  memoryBytes          33554432
  cpuSeconds           1.0
  wallClockSeconds     5.0
  outputBytes          1048576
  outputOverflow       DevelopGravity\LuaExt\OverflowBehavior::Fail
  maxLiveCoroutines    64
  maxCoroutineDepth    16
  maxCallDepth         200
  maxModules           64
  maxRequireDepth      16
  maxStringLength      67108864
  maxSourceBytes       1048576
  maxConversionDepth   64
  maxCachedChunks      64
  billHostTime         false
DevelopGravity\LuaExt\VfsQuota
  maxOpenHandles       16
  maxFileBytes         1048576
  maxTotalBytes        8388608
  maxFiles             128
  maxOperations        10000
  maxPathLength        255
  maxPathDepth         16
  billWallTime         false
DevelopGravity\LuaExt\SandboxConfig
  capabilities         NULL
  limits               NULL
  filesystem           NULL
  vfsQuota             NULL
  moduleResolver       NULL
  modulePaths          [/?.lua, /?/init.lua]
  outputMode           DevelopGravity\LuaExt\OutputMode::Buffer
  outputCallback       NULL
  outputChunkBytes     8192
  seed                 NULL
  deterministic        false
  cacheCompiledChunks  false
  sealMode             DevelopGravity\LuaExt\SealMode::Checksum
  bytecodeKey          NULL
NULL
NULL
bool(true)
bool(true)
