--TEST--
A script's __tostring cannot allocate without bound while its error is being described
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Exception\LuaThrowable;
use DevelopGravity\LuaExt\Limits;
use DevelopGravity\LuaExt\Sandbox;
use DevelopGravity\LuaExt\SandboxConfig;

// Describing an error must not fail for want of memory, so the message handler
// runs with headroom above the sandbox's ceiling. The trap is that describing
// an error calls tostring() on it, and a script chooses what that does: raise a
// value whose __tostring allocates, and the allocation happens under whatever
// the handler granted. Headroom is bounded for that reason -- an unlimited lift
// hands a script exactly the budget it was being stopped for.
//
// Nothing here needs a capability. error() and setmetatable() are in the
// untrusted baseline, so this is reachable from a default sandbox.

const CEILING = 4 * 1024 * 1024;

$run = static function (string $script) {
	$sandbox = new Sandbox(new SandboxConfig(limits: new Limits(memoryBytes: CEILING)));

	try {
		(void) $sandbox->eval($script, '=probe');
		$outcome = 'RAN';
	} catch (LuaThrowable $error) {
		$outcome = substr(strrchr($error::class, '\\'), 1);
	}

	$peak = $sandbox->stats()->peakMemoryBytes;
	$sandbox->close();

	return [$outcome, $peak];
};

// A __tostring that tries to build a string far past the ceiling. The headroom
// must not cover it, and the peak must stay near the ceiling rather than
// wherever the script decided to stop.
//
// The class that comes back is the ORIGINAL error, not a memory one: the
// allocation is refused inside the handler's own protected call, which treats a
// secondary failure as noise and reports what the script actually raised. The
// budget assertion beside it is the part that matters.
[$outcome, $peak] = $run(<<<'LUA'
	error(setmetatable({}, {
		__tostring = function() return ("x"):rep(64 * 1024 * 1024) end,
	}))
	LUA);

printf("greedy __tostring => %s, peak within budget: %s\n",
	$outcome,
	$peak <= CEILING * 2 ? 'yes' : sprintf('NO (%d bytes)', $peak));

// Repeating it must not accumulate: each error is described and released, so a
// hundred of them cost no more than one.
$sandbox = new Sandbox(new SandboxConfig(limits: new Limits(memoryBytes: CEILING)));

for ($attempt = 0; $attempt < 100; $attempt++) {
	try {
		(void) $sandbox->eval(<<<'LUA'
			error(setmetatable({}, {
				__tostring = function() return ("y"):rep(8 * 1024 * 1024) end,
			}))
			LUA, '=repeat');
	} catch (LuaThrowable) {
		// The outcome is asserted above; here only the accumulation matters.
	}
}

printf("100 attempts      => peak within budget: %s\n",
	$sandbox->stats()->peakMemoryBytes <= CEILING * 2 ? 'yes' : 'NO');
$sandbox->close();

// A message the script CAN afford is still not copied wholesale: what reaches
// the host is a bounded prefix, because that copy is persistent and outside
// every budget the host set.
$roomy = new Sandbox(new SandboxConfig(limits: new Limits(memoryBytes: 64 * 1024 * 1024)));

try {
	(void) $roomy->eval(<<<'LUA'
		error(setmetatable({}, {
			__tostring = function() return ("z"):rep(1024 * 1024) end,
		}))
		LUA, '=long');
	printf("long message      => RAN\n");
} catch (LuaThrowable $error) {
	printf("long message      => clamped: %s\n",
		strlen($error->getMessage()) <= 16384 ? 'yes' : sprintf('NO (%d bytes)', strlen($error->getMessage())));
}

$roomy->close();

// The reason the headroom exists at all still holds: a script that genuinely
// exhausts its budget gets a described error, not a silent failure.
[$outcome] = $run('local t = {} while true do t[#t + 1] = ("pad"):rep(1024) end');
printf("genuine exhaustion => %s\n", $outcome);

?>
--EXPECT--
greedy __tostring => RuntimeError, peak within budget: yes
100 attempts      => peak within budget: yes
long message      => clamped: yes
genuine exhaustion => MemoryLimitError
