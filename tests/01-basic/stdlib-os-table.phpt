--TEST--
The os table is built, not filtered: time and environment only, and nothing else
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Capabilities;
use DevelopGravity\LuaExt\Sandbox;
use DevelopGravity\LuaExt\SandboxConfig;

// loslib.c is not compiled, so this table has no upstream ancestor to filter --
// every member is one we wrote or ported. That is why the absence checks below
// are not paranoia about a deny-list: they assert that nothing crept back in.

/** The os table's members, sorted. */
function osMembers(Capabilities $capabilities): string
{
	$sandbox = new Sandbox(new SandboxConfig(capabilities: $capabilities));

	return $sandbox->eval(<<<'LUA'
		if os == nil then return "<nil>" end

		local names = {}
		for name in next, os do names[#names + 1] = name end
		table.sort(names)

		return table.concat(names, " ")
		LUA, '=os-members')[0];
}

$untrusted = Capabilities::untrusted();

// The table always exists, with at least os.clock, so a script never has to nil
// check it. os.clock is unconditional because it reports the sandbox's own
// billed CPU -- the quantity its own limit enforces.
printf("baseline   : %s\n", osMembers($untrusted));
printf("no osTime  : %s\n", osMembers($untrusted->with(osTime: false)));
printf("osEnv      : %s\n", osMembers($untrusted->with(osEnv: true)));
printf("neither    : %s\n", osMembers($untrusted->with(osTime: false, osEnv: false)));

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

// Everything that reaches outside the sandbox, or belongs to a wave that has not
// landed. os.execute, os.exit, os.tmpname and os.setlocale never arrive at all;
// os.remove and os.rename belong to the VFS.
$absent = $sandbox->eval(<<<'LUA'
	local names = {"execute", "exit", "tmpname", "setlocale", "remove", "rename", "getenv"}
	local present = {}

	for i = 1, #names do
		if rawget(os, names[i]) ~= nil then present[#present + 1] = names[i] end
	end

	return #present == 0 and "none present" or table.concat(present, " ")
	LUA, '=os-absent')[0];

var_dump($absent);

?>
--EXPECT--
baseline   : clock date difftime time
no osTime  : clock
osEnv      : clock date difftime getenv time
neither    : clock
clock nondecreasing=true advanced=true samples=4
bool(true)
string(6) "number"
float(600)
bool(true)
string(58) "bad argument #1 to '?' (invalid conversion specifier '%Q')"
string(12) "none present"
