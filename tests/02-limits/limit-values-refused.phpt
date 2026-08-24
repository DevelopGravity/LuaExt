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
 * API spells "no ceiling". So a value the conversion cannot represent must
 * never reach the cast: converting an out-of-range double to an integer is
 * undefined behaviour, and the result can be zero -- turning setCpuLimit(INF)
 * into a sandbox with no CPU limit at all. A limit silently becoming its own
 * absence is the failure this extension exists to eliminate.
 *
 * A value too large SATURATES rather than being refused, matching what a Limits
 * object does with the same number: a ceiling nobody will reach stays a ceiling
 * nobody will reach. It is only the values that are not deadlines at all --
 * negative, NAN, and zero -- that are refused here.
 *
 * NAN is caught by the same condition as the rest rather than a separate one,
 * because it compares false against everything: a check written as "reject if
 * <= 0" would let it through, and it too would reach the cast.
 */

$refusals = [
	'-INF' => -INF,
	'NAN' => NAN,
	'zero' => 0.0,
	'negative' => -1.0,
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

// A value past what nanoseconds can hold saturates instead of wrapping, and the
// sandbox stays usable. The danger this guards against is the opposite
// direction: a huge ceiling turning into a tiny one, or into none at all.
foreach (['setCpuLimit', 'setWallClockLimit'] as $method) {
	$sandbox = new Sandbox();
	$sandbox->{$method}(INF);
	$sandbox->{$method}(1.0e30);
	printf("%-18s saturates, sandbox still runs: %s\n", $method, $sandbox->eval('return "yes"')[0]);
	$sandbox->close();
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
setCpuLimit        -INF                               refused
setCpuLimit        NAN                                refused
setCpuLimit        zero                               refused
setCpuLimit        negative                           refused
setWallClockLimit  -INF                               refused
setWallClockLimit  NAN                                refused
setWallClockLimit  zero                               refused
setWallClockLimit  negative                           refused
setCpuLimit        saturates, sandbox still runs: yes
setWallClockLimit  saturates, sandbox still runs: yes
int(42)
string(11) "still works"
