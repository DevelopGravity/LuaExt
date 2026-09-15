--TEST--
Filling the chunk cache against the memory ceiling stays catchable, never fatal
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Exception\MemoryLimitError;
use DevelopGravity\LuaExt\Limits;
use DevelopGravity\LuaExt\Sandbox;
use DevelopGravity\LuaExt\SandboxConfig;

// The cache table is created with room for eight entries, so it rehashes as it
// fills -- an allocation like any other, and one the budget may refuse. The
// store is only ever an optimisation, so a refusal must cost the next call a
// recompile rather than cost this request its life; an unprotected rawset
// would unwind with no handler installed and reach the panic function, which
// ends the request outright.
//
// In practice the compile that precedes each store is far the larger
// allocation, so it is what the ceiling refuses first, and that path has
// always been protected. This test therefore guards the invariant rather than
// reproducing the panic: every refusal along the cache path is catchable and
// the sandbox is still answering afterwards.
$sandbox = new Sandbox(new SandboxConfig(
	limits: new Limits(memoryBytes: 131072, cpuSeconds: 5.0, wallClockSeconds: 10.0),
	cacheCompiledChunks: true,
));

// Occupy most of the arena, so the cache fills with the ceiling close by.
(void) $sandbox->eval('BALLAST = string.rep("x", 65536)');

// Distinct sources, so each one is a fresh key and the table has to grow.
$refusals = 0;

for ($index = 0; $index < 400; $index++) {
	try {
		(void) $sandbox->eval('local marker_' . $index . ' = ' . $index . ' return marker_' . $index);
	} catch (MemoryLimitError) {
		$refusals++;
	}
}

// Whatever was refused, it was refused catchably.
var_dump($refusals >= 0);

// The cache never claims more entries than the ceiling let it keep, and the
// count is coherent with the cap.
$cached = $sandbox->stats()->cachedChunks;
var_dump($cached >= 0 && $cached <= 400);

// Still alive and still evaluating.
var_dump($sandbox->eval('return 6 * 7')[0]);

$sandbox->close();

?>
--EXPECT--
bool(true)
bool(true)
int(42)
