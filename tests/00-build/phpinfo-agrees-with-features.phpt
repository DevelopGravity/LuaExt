--TEST--
The phpinfo() block reports the same enforcement levels features() does
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Sandbox;

// phpinfo() is where someone deploying this looks first, and it spent the whole
// project reporting "CPU limit enforcement: not implemented yet" -- long after
// the limit was real and the suite was asserting it. Nothing noticed, because
// nothing compared the two.
//
// Both now read luaext_limit_support_name() over the same probes. This is the
// check that keeps it that way: it fails if the strings drift, whichever side
// moves, and it fails on a platform where the probes disagree with themselves.

ob_start();
phpinfo(INFO_MODULES);
$info = ob_get_clean();

$features = Sandbox::features();

// phpinfo()'s HTML-free CLI output is "key => value" per line.
$row = static function (string $label) use ($info): ?string {
	foreach (explode("\n", $info) as $line) {
		[$key, $value] = array_pad(explode('=>', $line, 2), 2, null);

		if ($value !== null && trim($key) === $label) {
			return trim($value);
		}
	}

	return null;
};

$cpu = $row('CPU limit enforcement');
$wall = $row('Wall-clock limit enforcement');

printf("cpu row present:   %s\n", var_export($cpu !== null, true));
printf("wall row present:  %s\n", var_export($wall !== null, true));

printf("cpu agrees:        %s\n", var_export($cpu === $features['cpuLimit']->name, true));
printf("wall agrees:       %s\n", var_export($wall === $features['wallClockLimit']->name, true));

// The bytecode gate is a deployment fact an operator must be able to read off
// phpinfo() rather than by grepping php.ini.
printf("bytecode row:      %s\n", var_export($row('Unsealed bytecode'), true));

// The specific string this test was written to make unrepeatable.
printf("no stale claim:    %s\n", var_export(!str_contains($info, 'not implemented yet'), true));

// And the value is one of the three the enum actually has, rather than whatever
// an out-of-range support level would print.
printf("cpu is a real case: %s\n", var_export(
	in_array($cpu, ['Enforced', 'Degraded', 'Unsupported'], true),
	true,
));

?>
--EXPECT--
cpu row present:   true
wall row present:  true
cpu agrees:        true
wall agrees:       true
bytecode row:      'refused'
no stale claim:    true
cpu is a real case: true
