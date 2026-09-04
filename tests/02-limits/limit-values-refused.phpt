--TEST--
A time limit that cannot be represented is refused, never quietly turned into none
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Exception\ConfigurationError;
use DevelopGravity\LuaExt\Limits;
use DevelopGravity\LuaExt\Sandbox;

/*
 * Seconds become nanoseconds, and zero nanoseconds is how this API spells "no
 * ceiling". So a value the conversion cannot represent must never reach the
 * cast: converting an out-of-range double to an integer is undefined behaviour,
 * and the result can be zero -- turning a CPU limit of INF into a sandbox with
 * no CPU limit at all. A limit silently becoming its own absence is the failure
 * this extension exists to eliminate.
 *
 * A value too large SATURATES rather than being refused: a ceiling nobody will
 * reach stays a ceiling nobody will reach. It is only the values that are not
 * deadlines at all -- negative and NAN -- that are refused.
 *
 * NAN is caught by the same condition as the rest rather than a separate one,
 * because it compares false against everything: a check written as "reject if
 * <= 0" would let it through, and it too would reach the cast.
 *
 * ZERO IS NOT REFUSED HERE, and that is a deliberate change. It used to be, by
 * setCpuLimit() only -- while the Limits object the constructor took read the
 * same 0.0 as "no ceiling". One number meant two things depending on which door
 * it came through. There is one door now, and it is the constructor's rules.
 */

$refusals = [
	'-INF' => -INF,
	'NAN' => NAN,
	'negative' => -1.0,
];

$fields = [
	'cpuSeconds' => static fn (Limits $l, float $s): Limits => $l->with(cpuSeconds: $s),
	'wallClockSeconds' => static fn (Limits $l, float $s): Limits => $l->with(wallClockSeconds: $s),
];

foreach ($fields as $field => $build) {
	foreach ($refusals as $label => $seconds) {
		$sandbox = new Sandbox();

		try {
			$sandbox->setLimits($build($sandbox->limits(), $seconds));
			printf("%-18s %-34s ACCEPTED\n", $field, $label);
		} catch (ConfigurationError) {
			printf("%-18s %-34s refused\n", $field, $label);
		} finally {
			$sandbox->close();
		}
	}
}

// A value past what nanoseconds can hold saturates instead of wrapping, and the
// sandbox stays usable. The danger this guards against is the opposite
// direction: a huge ceiling turning into a tiny one, or into none at all.
foreach ($fields as $field => $build) {
	$sandbox = new Sandbox();
	$sandbox->setLimits($build($sandbox->limits(), INF));
	$sandbox->setLimits($build($sandbox->limits(), 1.0e30));
	printf("%-18s saturates, sandbox still runs: %s\n", $field, $sandbox->eval('return "yes"')[0]);
	$sandbox->close();
}

// null lifts a deadline, and so does 0.0 -- the two spellings the Limits object
// has always collapsed together.
$sandbox = new Sandbox();
$sandbox->setLimits($sandbox->limits()->with(cpuSeconds: null, wallClockSeconds: null));
var_dump($sandbox->limits()->cpuSeconds, $sandbox->limits()->wallClockSeconds);
$sandbox->setLimits($sandbox->limits()->with(cpuSeconds: 0.0, wallClockSeconds: 0.0));
var_dump($sandbox->limits()->cpuSeconds, $sandbox->limits()->wallClockSeconds);
var_dump($sandbox->eval('return 6 * 7')[0]);
$sandbox->close();

// A representable limit is still accepted, still enforced, and reads back.
$sandbox = new Sandbox();
$sandbox->setLimits($sandbox->limits()->with(cpuSeconds: 0.5, wallClockSeconds: 2.0));
var_dump($sandbox->limits()->cpuSeconds, $sandbox->limits()->wallClockSeconds);
var_dump($sandbox->eval('return "still works"')[0]);
$sandbox->close();

?>
--EXPECT--
cpuSeconds         -INF                               refused
cpuSeconds         NAN                                refused
cpuSeconds         negative                           refused
wallClockSeconds   -INF                               refused
wallClockSeconds   NAN                                refused
wallClockSeconds   negative                           refused
cpuSeconds         saturates, sandbox still runs: yes
wallClockSeconds   saturates, sandbox still runs: yes
NULL
NULL
NULL
NULL
int(42)
float(0.5)
float(2)
string(11) "still works"
