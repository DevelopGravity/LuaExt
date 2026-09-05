--TEST--
The debug table is assembled from the debug capabilities: real where granted, gates where not
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Capabilities;
use DevelopGravity\LuaExt\Exception\FeatureNotGrantedError;
use DevelopGravity\LuaExt\Limits;
use DevelopGravity\LuaExt\Sandbox;
use DevelopGravity\LuaExt\SandboxConfig;

// Which members are REAL is the whole security property of this library. The
// names no longer vary with the capabilities -- a withheld member is a gate
// stub that raises FeatureNotGrantedError -- so membership listings prove
// nothing and every claim below is made by CALLING. The two that read like
// introspection but are filed under debugMutate -- getmetatable and
// setmetatable -- are the ones a future edit is most likely to get wrong,
// because they bypass __metatable and Capabilities::trusted() grants
// debugIntrospect.

/** works / gate(<capability>) for each debug member, under the given caps. */
function debugBehaviour(Capabilities $capabilities): string
{
	// BOTH time limits are lifted, not just the CPU one. debugHooks is refused
	// alongside either: a script that can call debug.sethook() displaces the
	// interpreter hook they are both delivered through.
	$sandbox = new Sandbox(new SandboxConfig(
		capabilities: $capabilities,
		limits: new Limits(cpuSeconds: null, wallClockSeconds: null),
	));

	if ($sandbox->eval('return type(debug)', '=probe')[0] === 'nil') {
		$sandbox->close();

		return '<nil>';
	}

	// Arguments chosen so a REAL member returns rather than argue; an argument
	// squabble would still be a catchable RuntimeError, which classifies as
	// "works" -- the member ran its own code.
	$calls = [
		'traceback' => 'return debug.traceback("x")',
		'getinfo' => 'return debug.getinfo(1) ~= nil',
		'getlocal' => 'return debug.getlocal(1, 1)',
		'getupvalue' => 'local u = 1 local f = function() return u end return debug.getupvalue(f, 1)',
		'getmetatable' => 'return debug.getmetatable({}) == nil',
		'setmetatable' => 'return debug.setmetatable({}, nil) ~= nil',
		'getregistry' => 'return debug.getregistry() ~= nil',
		'sethook' => 'debug.sethook() return true',
		'gethook' => 'return debug.gethook() == nil',
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

printf("none         : %s\n", debugBehaviour($untrusted->with(debugTraceback: false)));
printf("traceback    : %s\n", debugBehaviour($untrusted));
printf("introspect   : %s\n", debugBehaviour($untrusted->with(debugIntrospect: true)));
printf("mutate       : %s\n", debugBehaviour($untrusted->with(debugMutate: true)));
printf("hooks        : %s\n", debugBehaviour($untrusted->with(debugHooks: true)));
printf("everything   : %s\n", debugBehaviour($untrusted->with(
	debugIntrospect: true, debugMutate: true, debugHooks: true)));

// With no debug capability the global is nil -- tier 1 of the classification --
// and touching it says which capability opens the door.
$none = new Sandbox(new SandboxConfig(capabilities: $untrusted->with(debugTraceback: false)));
var_dump($none->eval('return type(debug)', '=debug-type')[0]);

try {
	(void) $none->eval('return debug.traceback()', '=debug-touch');
	echo "TOUCHED A NIL DEBUG\n";
} catch (FeatureNotGrantedError $error) {
	echo $error->getMessage(), "\n";
}

$none->close();

// trusted() grants debugIntrospect and not debugMutate, so getmetatable and
// setmetatable must be GATES there: if they ran, every trusted sandbox could
// pull the metatable off an error userdata and rewrite __tostring.
// vfs is dropped only because it needs a FileSystem to construct.
$trusted = new Sandbox(new SandboxConfig(
	capabilities: Capabilities::trusted()->with(vfs: false, vfsWrite: false),
));

try {
	(void) $trusted->eval('return debug.getmetatable({})', '=trusted-metatable');
	echo "TRUSTED REACHED getmetatable\n";
} catch (FeatureNotGrantedError) {
	echo "trusted getmetatable is a gate\n";
}

var_dump($trusted->eval('return debug.getinfo(1) ~= nil', '=trusted-introspect')[0]);

try {
	(void) $trusted->eval('return debug.getregistry()', '=trusted-registry');
	echo "TRUSTED REACHED getregistry\n";
} catch (FeatureNotGrantedError) {
	echo "trusted getregistry is a gate\n";
}

$trusted->close();

// An interactive REPL reading stdin is never appropriate, at any level, so
// debug.debug is wholly absent -- not even a gate, because no capability could
// ever grant it.
$everything = new Sandbox(new SandboxConfig(
	capabilities: $untrusted->with(debugIntrospect: true, debugMutate: true, debugHooks: true),
	limits: new Limits(cpuSeconds: null, wallClockSeconds: null),
));

var_dump($everything->eval('return debug.debug == nil', '=debug-repl')[0]);

$everything->close();

?>
--EXPECT--
none         : <nil>
traceback    : traceback getinfo=gate(debugIntrospect) getlocal=gate(debugIntrospect) getupvalue=gate(debugIntrospect) getmetatable=gate(debugMutate) setmetatable=gate(debugMutate) getregistry=gate(debugMutate) sethook=gate(debugHooks) gethook=gate(debugHooks)
introspect   : traceback getinfo getlocal getupvalue getmetatable=gate(debugMutate) setmetatable=gate(debugMutate) getregistry=gate(debugMutate) sethook=gate(debugHooks) gethook=gate(debugHooks)
mutate       : traceback getinfo=gate(debugIntrospect) getlocal=gate(debugIntrospect) getupvalue=gate(debugIntrospect) getmetatable setmetatable getregistry sethook=gate(debugHooks) gethook=gate(debugHooks)
hooks        : traceback getinfo=gate(debugIntrospect) getlocal=gate(debugIntrospect) getupvalue=gate(debugIntrospect) getmetatable=gate(debugMutate) setmetatable=gate(debugMutate) getregistry=gate(debugMutate) sethook gethook
everything   : traceback getinfo getlocal getupvalue getmetatable setmetatable getregistry sethook gethook
string(3) "nil"
The script used debug, which needs the debugTraceback capability this sandbox was not granted
trusted getmetatable is a gate
bool(true)
trusted getregistry is a gate
bool(true)
