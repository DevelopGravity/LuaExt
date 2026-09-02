--TEST--
SandboxStats is host-unbuildable and serialises under its documented field names
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\SandboxStats;

// A host cannot build a snapshot: one that nothing measured would be
// indistinguishable from a real one in a log.
try {
	new SandboxStats();
} catch (Error $error) {
	printf("%s: %s\n", $error::class, $error->getMessage());
}

var_dump((new ReflectionClass(SandboxStats::class))->implementsInterface(JsonSerializable::class));

/*
 * Reached through reflection because Sandbox::stats() has not landed yet -- it
 * belongs to the watchdog and allocator subsystems. What is being pinned here
 * is the JSON contract: the field names and their types, which logging and
 * billing pipelines index by.
 */
$stats = (new ReflectionClass(SandboxStats::class))->newInstanceWithoutConstructor();
(new ReflectionMethod($stats, '__construct'))->invoke($stats);

$serialised = $stats->jsonSerialize();

foreach ($serialised as $field => $value) {
	printf("%-20s %s\n", $field, get_debug_type($value));
}

// The array is exactly the declared properties, in declaration order.
var_dump(array_keys($serialised) === array_map(
	static fn (ReflectionProperty $property): string => $property->getName(),
	(new ReflectionObject($stats))->getProperties(),
));

echo json_encode($stats), "\n";

?>
--EXPECT--
Error: Call to private DevelopGravity\LuaExt\SandboxStats::__construct() from global scope
bool(true)
memoryBytes          int
peakMemoryBytes      int
memoryLimitBytes     int
cpuSeconds           float
wallClockSeconds     float
outputBytes          int
outputTruncated      bool
liveCoroutines       int
peakCoroutineDepth   int
modulesLoaded        int
cachedChunks         int
vfsOperations        int
vfsBytes             int
gcCollections        int
luaCallsIn           int
phpCallsOut          int
bool(true)
{"memoryBytes":0,"peakMemoryBytes":0,"memoryLimitBytes":0,"cpuSeconds":0,"wallClockSeconds":0,"outputBytes":0,"outputTruncated":false,"liveCoroutines":0,"peakCoroutineDepth":0,"modulesLoaded":0,"cachedChunks":0,"vfsOperations":0,"vfsBytes":0,"gcCollections":0,"luaCallsIn":0,"phpCallsOut":0}
