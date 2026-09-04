--TEST--
Sandbox::setLimits() re-ceilings a live sandbox without unwinding it
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Exception\ConfigurationError;
use DevelopGravity\LuaExt\Limits;
use DevelopGravity\LuaExt\Sandbox;

// There were three setters here once -- setMemoryLimit, setCpuLimit and
// setWallClockLimit -- and between them they reached three of the fourteen
// limits a Limits object carries. The other eleven were read live off the policy
// the whole time, so they were always changeable and simply had no door.
//
// Now there is one door, taking the same object the constructor takes.

$sandbox = new Sandbox();

$usage = $sandbox->stats()->memoryBytes;
$peak = $sandbox->stats()->peakMemoryBytes;

// The round trip is the point: read, change one field, write back.
$defaults = $sandbox->limits();
printf(
	"defaults: memory=%d cpu=%s wall=%s callDepth=%d\n",
	$defaults->memoryBytes,
	var_export($defaults->cpuSeconds, true),
	var_export($defaults->wallClockSeconds, true),
	$defaults->maxCallDepth,
);

// Raising the ceiling is uneventful.
$sandbox->setLimits($defaults->with(memoryBytes: 64 * 1024 * 1024));
var_dump($sandbox->stats()->memoryBytes === $usage);

// Null lifts it. So does 0 -- which the old setter refused, for no reason a
// caller could have discovered: the constructor accepted the same 0 and read it
// the same way. One number, one meaning, whichever door it comes through.
$sandbox->setLimits($defaults->with(memoryBytes: null));
var_dump($sandbox->limits()->memoryBytes === null);
$sandbox->setLimits($defaults->with(memoryBytes: 0));
var_dump($sandbox->limits()->memoryBytes === null);

// A field the old API could not reach at all, changed and read back.
$sandbox->setLimits($defaults->with(maxCallDepth: 7, maxCachedChunks: 3));
printf(
	"reachable now: callDepth=%d cachedChunks=%d\n",
	$sandbox->limits()->maxCallDepth,
	$sandbox->limits()->maxCachedChunks,
);

// The interesting case: a ceiling below what the sandbox already holds. This
// must not fail retroactively -- the memory is allocated and in use, and there
// is nothing safe to unwind. Only the next allocation that would grow the heap
// is refused, which nothing here attempts.
$sandbox->setLimits($defaults->with(memoryBytes: 1024));
var_dump($sandbox->stats()->memoryBytes === $usage);
var_dump($sandbox->stats()->peakMemoryBytes === $peak);

// Down to a single byte, and the sandbox is still perfectly usable.
$sandbox->setLimits($defaults->with(memoryBytes: 1));
var_dump($sandbox->stats()->memoryBytes === $usage);
var_dump($sandbox->isClosed());

// ...and still closable, which matters most: a sandbox held below its own
// footprint must not become impossible to tear down.
$sandbox->close();
var_dump($sandbox->isClosed());

// A value the limit cannot represent is refused by the same function the
// constructor uses, so it is refused with the same words. That is the whole
// reason setLimits() does not do its own checking.
$live = new Sandbox();
$base = $live->limits();

$rejected = [
	'memoryBytes negative' => static fn (Limits $l): Limits => $l->with(memoryBytes: -1),
	'cpuSeconds negative' => static fn (Limits $l): Limits => $l->with(cpuSeconds: -1.0),
	'cpuSeconds NAN' => static fn (Limits $l): Limits => $l->with(cpuSeconds: NAN),
	'maxCallDepth negative' => static fn (Limits $l): Limits => $l->with(maxCallDepth: -5),
];

foreach ($rejected as $label => $build) {
	try {
		$live->setLimits($build($base));
		printf("%s: NOT REFUSED\n", $label);
	} catch (ConfigurationError $error) {
		printf("%s: %s\n", $label, $error->getMessage());
	}
}

// A rejected Limits changes nothing: the read happens into a local, and the
// sandbox is only moved once every field has been accepted.
var_dump($live->limits()->memoryBytes === $base->memoryBytes);
var_dump($live->limits()->maxCallDepth === $base->maxCallDepth);
var_dump($live->stats()->memoryBytes > 0);

$live->close();

?>
--EXPECT--
defaults: memory=33554432 cpu=1.0 wall=5.0 callDepth=200
bool(true)
bool(true)
bool(true)
reachable now: callDepth=7 cachedChunks=3
bool(true)
bool(true)
bool(true)
bool(false)
bool(true)
memoryBytes negative: Limits::$memoryBytes is -1, and a limit cannot be negative. Pass 0 (or null, where the type allows it) to lift the limit, or a positive value to set one.
cpuSeconds negative: Limits::$cpuSeconds is -1, which is not a number of seconds a deadline can be set from. Pass null (or 0.0) to lift the limit, or a positive number of seconds to set one.
cpuSeconds NAN: Limits::$cpuSeconds is NAN, which is not a number of seconds a deadline can be set from. Pass null (or 0.0) to lift the limit, or a positive number of seconds to set one.
maxCallDepth negative: Limits::$maxCallDepth is -5, and a limit cannot be negative. Pass 0 (or null, where the type allows it) to lift the limit, or a positive value to set one.
bool(true)
bool(true)
bool(true)
