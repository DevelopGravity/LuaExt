--TEST--
collectgarbage's tuning verbs need gcControl and its reading verbs do not
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Capabilities;
use DevelopGravity\LuaExt\Sandbox;
use DevelopGravity\LuaExt\SandboxConfig;

// Why the tuning verbs are more dangerous here than they are upstream: the
// allocator installs GC parameters in tiers as usage approaches memoryBytes and
// caches which tier is installed, so one collectgarbage("param", ...) overwrites
// them while the allocator still believes its tuning is in place -- and never
// reinstalls it for the life of that sandbox.
//
// "collect" is deliberately NOT one of them. A full collection only ever frees
// more than it started with.

$reading = ['collect', 'count', 'step', 'isrunning'];
$tuning = ['stop', 'restart', 'generational', 'incremental', 'param'];

foreach ([false, true] as $granted) {
	$sandbox = new Sandbox(new SandboxConfig(
		capabilities: (new Capabilities())->with(gcControl: $granted),
	));

	printf("gcControl=%s\n", var_export($granted, true));

	foreach ([...$reading, ...$tuning] as $verb) {
		$arguments = $verb === 'param' ? '"param", "pause"' : sprintf('%s', var_export($verb, true));

		[$ok, $answer] = $sandbox->eval(sprintf(
			'local ok, result = pcall(collectgarbage, %s)
			return ok, ok and "ok" or tostring(result)',
			$arguments,
		));

		printf("  %-13s %s\n", $verb, $ok ? $answer : $answer);
	}

	// An unknown verb reads as an unknown verb, and the message does not
	// enumerate the ones being withheld -- an error message is not a place to
	// publish the shape of what a script did not get.
	[, $unknown] = $sandbox->eval(
		'local ok, result = pcall(collectgarbage, "frobnicate") return ok, tostring(result)',
	);

	printf("  %-13s %s\n", 'frobnicate', str_contains($unknown, 'generational') ? 'LEAKS THE LIST' : $unknown);

	$sandbox->close();
}

?>
--EXPECT--
gcControl=false
  collect       ok
  count         ok
  step          ok
  isrunning     ok
  stop          collectgarbage("stop") needs the gcControl capability, which this sandbox was not given
  restart       collectgarbage("restart") needs the gcControl capability, which this sandbox was not given
  generational  collectgarbage("generational") needs the gcControl capability, which this sandbox was not given
  incremental   collectgarbage("incremental") needs the gcControl capability, which this sandbox was not given
  param         collectgarbage("param") needs the gcControl capability, which this sandbox was not given
  frobnicate    bad argument #1 to '?' (invalid option 'frobnicate')
gcControl=true
  collect       ok
  count         ok
  step          ok
  isrunning     ok
  stop          ok
  restart       ok
  generational  ok
  incremental   ok
  param         ok
  frobnicate    bad argument #1 to '?' (invalid option 'frobnicate')
