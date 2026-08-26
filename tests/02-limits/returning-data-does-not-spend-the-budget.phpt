--TEST--
Returning values to the host does not permanently spend the memory budget
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Limits;
use DevelopGravity\LuaExt\Sandbox;
use DevelopGravity\LuaExt\SandboxConfig;

// Converting a Lua value for the host allocates a PHP copy, and the converter
// MEASURES that copy on every path. Whether it is CHARGED against the memory
// limit is a separate decision, made by whichever boundary owns the resulting
// zval's lifetime:
//
//   the callback bridge  owns its params and frees them when the call returns,
//                        so it bills and discharges.
//   a result / a global  is handed to PHP, whose lifetime the extension neither
//                        knows nor controls -- so it must NOT bill, because
//                        nothing would ever give the budget back.
//
// This pins the second rule. It is not hypothetical: the results path built its
// context without assigning `bill` at all, leaving it stack garbage, so whether
// results were charged depended on what happened to be in that slot. Forced to
// the wrong value it burned ~256 KiB per call and never returned it -- a sandbox
// that worked, then gradually refused to.

$sandbox = new Sandbox(new SandboxConfig(limits: new Limits(memoryBytes: 4 * 1024 * 1024)));

$script = 'local t = {} for i = 1, 2000 do t[i] = string.rep("x", 64) end return t';

$readings = [];

for ($call = 0; $call < 8; $call++) {
	[$returned] = $sandbox->eval($script, '=results');

	// Dropped immediately: the point is what the SANDBOX still counts as live,
	// not what PHP happens to be holding.
	unset($returned);

	$readings[] = $sandbox->stats()->memoryBytes;
}

// Lua's own collector makes the exact figure move a little, so the assertion is
// on the trend rather than on equality: eight calls that each leaked their
// result would land far above where the first one did.
$first = $readings[0];
$last = $readings[count($readings) - 1];
$peak = max($readings);

printf("grew across calls: %s\n", var_export($last > $first * 2, true));
printf("peak within twice the first: %s\n", var_export($peak <= $first * 2, true));

$sandbox->close();

?>
--EXPECT--
grew across calls: false
peak within twice the first: true
