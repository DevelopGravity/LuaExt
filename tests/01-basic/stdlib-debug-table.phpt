--TEST--
The debug table is assembled from the debug capabilities, and is nil without any
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Capabilities;
use DevelopGravity\LuaExt\Limits;
use DevelopGravity\LuaExt\Sandbox;
use DevelopGravity\LuaExt\SandboxConfig;

// Membership is the whole security property of this library. Every name here
// was placed deliberately, and the two that read like introspection but are
// filed under debugMutate -- getmetatable and setmetatable -- are the ones a
// future edit is most likely to get wrong, because they bypass __metatable and
// Capabilities::trusted() grants debugIntrospect.

/** The debug table's members, sorted, or `<nil>` when there is no table. */
function debugMembers(Capabilities $capabilities): string
{
	// BOTH time limits are lifted, not just the CPU one. debugHooks is refused
	// alongside either: a script that can call debug.sethook() displaces the
	// interpreter hook they are both delivered through -- the watchdog thread
	// only raises a flag, and that hook is what turns the flag into a stopped
	// script -- so a wall limit would stop being enforced just as surely.
	$sandbox = new Sandbox(new SandboxConfig(
		capabilities: $capabilities,
		limits: new Limits(cpuSeconds: null, wallClockSeconds: null),
	));

	return $sandbox->eval(<<<'LUA'
		if debug == nil then return "<nil>" end

		local names = {}
		for name in next, debug do names[#names + 1] = name end
		table.sort(names)

		return table.concat(names, " ")
		LUA, '=debug-members')[0];
}

$untrusted = Capabilities::untrusted();

printf("none         : %s\n", debugMembers($untrusted->with(debugTraceback: false)));
printf("traceback    : %s\n", debugMembers($untrusted));
printf("introspect   : %s\n", debugMembers($untrusted->with(debugIntrospect: true)));
printf("mutate       : %s\n", debugMembers($untrusted->with(debugMutate: true)));
printf("hooks        : %s\n", debugMembers($untrusted->with(debugHooks: true)));
printf("everything   : %s\n", debugMembers($untrusted->with(
	debugIntrospect: true, debugMutate: true, debugHooks: true)));

// With no debug capability the global must be nil, not an empty table: a script
// asking `if debug then` is asking whether it has the library, and {} answers
// yes to a question whose true answer is no.
$none = new Sandbox(new SandboxConfig(capabilities: $untrusted->with(debugTraceback: false)));
var_dump($none->eval('return type(debug)', '=debug-type')[0]);

// trusted() grants debugIntrospect and not debugMutate, so it must NOT reach
// getmetatable/setmetatable. If it did, every trusted sandbox would be able to
// pull the metatable off an error userdata and rewrite __tostring.
// vfs is dropped only because it needs a FileSystem to construct.
$trusted = new Sandbox(new SandboxConfig(
	capabilities: Capabilities::trusted()->with(vfs: false, vfsWrite: false),
));

var_dump($trusted->eval(
	'return debug.getmetatable == nil and debug.setmetatable == nil', '=trusted-metatable')[0]);
var_dump($trusted->eval('return debug.getinfo ~= nil', '=trusted-introspect')[0]);
var_dump($trusted->eval('return debug.getregistry == nil', '=trusted-registry')[0]);

// An interactive REPL reading stdin is never appropriate, at any level, so
// debug.debug is withheld even from a sandbox holding every debug capability.
$everything = new Sandbox(new SandboxConfig(
	capabilities: $untrusted->with(debugIntrospect: true, debugMutate: true, debugHooks: true),
	limits: new Limits(cpuSeconds: null, wallClockSeconds: null),
));

var_dump($everything->eval('return debug.debug == nil', '=debug-repl')[0]);

?>
--EXPECT--
none         : <nil>
traceback    : traceback
introspect   : getinfo getlocal getupvalue traceback
mutate       : getmetatable getregistry getuservalue setlocal setmetatable setupvalue setuservalue traceback upvalueid upvaluejoin
hooks        : gethook sethook traceback
everything   : gethook getinfo getlocal getmetatable getregistry getupvalue getuservalue sethook setlocal setmetatable setupvalue setuservalue traceback upvalueid upvaluejoin
string(3) "nil"
bool(true)
bool(true)
bool(true)
bool(true)
