--TEST--
The os table is built, not filtered: real functions where granted, gate stubs where not
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Capabilities;
use DevelopGravity\LuaExt\Exception\FeatureNotGrantedError;
use DevelopGravity\LuaExt\Sandbox;
use DevelopGravity\LuaExt\SandboxConfig;

// loslib.c is not compiled, so this table has no upstream ancestor to filter --
// every member is one we wrote or ported. The table's NAMES no longer vary
// with capabilities: a withheld member is a gate stub that raises
// FeatureNotGrantedError when called, so the question a preset answers is not
// "what exists" but "what runs". Classified by calling, which is the only
// probe that tells a stub from the real thing.

/** works / gate(<capability>) for each os member, under the given caps. */
function osBehaviour(Capabilities $capabilities): string
{
	$sandbox = new Sandbox(new SandboxConfig(capabilities: $capabilities));

	$calls = [
		'clock' => 'return os.clock()',
		'time' => 'return os.time()',
		'date' => 'return os.date("!%Y")',
		'difftime' => 'return os.difftime(2, 1)',
		'getenv' => 'return os.getenv("PATH")',
	];

	$report = [];

	foreach ($calls as $name => $script) {
		try {
			(void) $sandbox->eval($script, '=probe');
			$report[] = $name;
		} catch (FeatureNotGrantedError $error) {
			preg_match('/needs the (\w+) capability/', $error->getMessage(), $match);
			$report[] = sprintf('%s=gate(%s)', $name, $match[1] ?? '?');
		}
	}

	$sandbox->close();

	return implode(' ', $report);
}

$untrusted = Capabilities::untrusted();

printf("baseline   : %s\n", osBehaviour($untrusted));
printf("no osTime  : %s\n", osBehaviour($untrusted->with(osTime: false)));
printf("osEnv      : %s\n", osBehaviour($untrusted->with(osEnv: true)));
printf("neither    : %s\n", osBehaviour($untrusted->with(osTime: false, osEnv: false)));

$sandbox = new Sandbox();

// Never a constant. A frozen clock surfaces as mysterious script bugs rather
// than as a missing feature, so it has to move and it has to move forwards.
$readings = $sandbox->eval(<<<'LUA'
	local samples = {}

	for round = 1, 4 do
		local sum = 0
		for i = 1, 300000 do sum = sum + i end
		samples[#samples + 1] = os.clock()
	end

	local nondecreasing = true
	for i = 2, #samples do
		if samples[i] < samples[i - 1] then nondecreasing = false end
	end

	return {nondecreasing, samples[#samples] > 0, #samples}
	LUA, '=os-clock')[0];

printf("clock nondecreasing=%s advanced=%s samples=%d\n",
	var_export($readings[1], true), var_export($readings[2], true), $readings[3]);

// A '!' format is UTC, which is the only one whose answer does not depend on the
// host's time zone. A non-'!' one lets a script infer that time zone, which is
// an accepted disclosure at the untrusted baseline.
$year = $sandbox->eval('return os.date("!%Y")', '=os-date')[0];
var_dump(strlen($year) === 4 && (int) $year >= 2020 && (int) $year <= 2200);

var_dump($sandbox->eval('return type(os.date("*t").year)', '=os-date-table')[0]);
var_dump($sandbox->eval('return os.difftime(1000, 400)', '=os-difftime')[0]);
var_dump($sandbox->eval('return os.time({year = 2000, month = 1, day = 1}) > 0', '=os-time')[0]);

// checkoption's allow-list is the security-relevant half of the port: strftime's
// behaviour on a specifier the platform does not implement is undefined.
var_dump($sandbox->eval(
	'return select(2, pcall(os.date, "%Q"))', '=os-date-bad')[0]);

// The functions that reach outside the sandbox never arrive at all -- not even
// as gates, because no capability could ever grant them. os.remove, os.rename
// and os.getenv are deliberately NOT in this list any more: they exist as gate
// stubs so a call can say which capability is missing.
$absent = $sandbox->eval(<<<'LUA'
	local names = {"execute", "exit", "tmpname", "setlocale"}
	local present = {}

	for i = 1, #names do
		if rawget(os, names[i]) ~= nil then present[#present + 1] = names[i] end
	end

	return #present == 0 and "none present" or table.concat(present, " ")
	LUA, '=os-absent')[0];

var_dump($absent);

// The gates are truthy functions -- the accepted trade for classification --
// so member-level truthiness is NOT a feature probe. Pinned so it cannot
// drift silently in either direction.
var_dump($sandbox->eval('return type(os.getenv)', '=os-gate-type')[0]);

$sandbox->close();

?>
--EXPECT--
baseline   : clock time date difftime getenv=gate(osEnv)
no osTime  : clock time=gate(osTime) date=gate(osTime) difftime=gate(osTime) getenv=gate(osEnv)
osEnv      : clock time date difftime getenv
neither    : clock time=gate(osTime) date=gate(osTime) difftime=gate(osTime) getenv=gate(osEnv)
clock nondecreasing=true advanced=true samples=4
bool(true)
string(6) "number"
float(600)
bool(true)
string(58) "bad argument #1 to '?' (invalid conversion specifier '%Q')"
string(12) "none present"
string(8) "function"
