--TEST--
A time limit that cannot be represented is refused, never quietly turned into none
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Sandbox;

/*
 * Both setters convert seconds to nanoseconds, and zero nanoseconds is how this
 * API spells "no ceiling". So every value that cannot be converted has to be
 * refused at the boundary rather than cast: converting an out-of-range double
 * to an integer is undefined behaviour, and the shape it usually takes is a
 * saturated or wrapped value. Landing on zero would turn setCpuLimit(INF) into
 * a sandbox with no CPU limit at all -- a limit silently becoming its own
 * absence, which is the failure this extension exists to eliminate.
 *
 * NAN is refused by the same condition rather than a separate one, because it
 * compares false against everything: a test written as "reject if <= 0" would
 * let it through, and it too would reach the cast.
 */

$refusals = [
	'INF' => INF,
	'-INF' => -INF,
	'NAN' => NAN,
	'zero' => 0.0,
	'negative' => -1.0,
	'far past 584 years of nanoseconds' => 1.0e30,
];

foreach (['setCpuLimit', 'setWallClockLimit'] as $method) {
	foreach ($refusals as $label => $seconds) {
		$sandbox = new Sandbox();

		try {
			$sandbox->{$method}($seconds);
			printf("%-18s %-34s ACCEPTED\n", $method, $label);
		} catch (ValueError) {
			printf("%-18s %-34s refused\n", $method, $label);
		} finally {
			$sandbox->close();
		}
	}
}

// null is the one spelling of "no ceiling", and it is still accepted.
$sandbox = new Sandbox();
$sandbox->setCpuLimit(null);
$sandbox->setWallClockLimit(null);
var_dump($sandbox->eval('return 6 * 7')[0]);
$sandbox->close();

// A representable limit is still accepted, and still enforced.
$sandbox = new Sandbox();
$sandbox->setCpuLimit(0.5);
$sandbox->setWallClockLimit(2.0);
var_dump($sandbox->eval('return "still works"')[0]);
$sandbox->close();

?>
--EXPECT--
setCpuLimit        INF                                refused
setCpuLimit        -INF                               refused
setCpuLimit        NAN                                refused
setCpuLimit        zero                               refused
setCpuLimit        negative                           refused
setCpuLimit        far past 584 years of nanoseconds  refused
setWallClockLimit  INF                                refused
setWallClockLimit  -INF                               refused
setWallClockLimit  NAN                                refused
setWallClockLimit  zero                               refused
setWallClockLimit  negative                           refused
setWallClockLimit  far past 584 years of nanoseconds  refused
int(42)
string(11) "still works"
