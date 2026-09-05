--TEST--
os.getenv answers only from the allow list, and cannot be used to probe it
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Capabilities;
use DevelopGravity\LuaExt\Sandbox;
use DevelopGravity\LuaExt\SandboxConfig;

// The allow list is materialised into a Lua set once, at construction. os.getenv
// touches no zval at call time, so nothing a script does can make it read a PHP
// property, run host code, or observe a change made after the sandbox was built.

putenv('LUAEXT_TEST_VISIBLE=visible value');
putenv('LUAEXT_TEST_HIDDEN=hidden value');

$sandbox = new Sandbox(new SandboxConfig(
	capabilities: Capabilities::untrusted()->with(
		osEnv: true,
		osEnvAllowList: ['LUAEXT_TEST_VISIBLE', 'LUAEXT_TEST_ABSENT'],
	),
));

/** Run `$expression` and print what came back. */
function probe(Sandbox $sandbox, string $expression): void
{
	printf("%-44s => %s\n", $expression, var_export($sandbox->eval("return $expression", '=probe')[0], true));
}

probe($sandbox, 'os.getenv("LUAEXT_TEST_VISIBLE")');

// Set in the environment, absent from the allow list. The answer must be
// indistinguishable from a variable that is simply not set: if a script could
// tell "you may not read this" from "there is nothing to read", the allow list
// would itself be a disclosure channel.
probe($sandbox, 'os.getenv("LUAEXT_TEST_HIDDEN")');
probe($sandbox, 'os.getenv("LUAEXT_TEST_ABSENT")');
probe($sandbox, 'os.getenv("LUAEXT_TEST_NEVER_LISTED")');
probe($sandbox, 'os.getenv("PATH")');

// A NUL truncates the C string handed to getenv(), so a name carrying one could
// resolve to a different variable than the one the allow list was checked
// against. Refused before the lookup, so the refusal reveals nothing either.
probe($sandbox, 'select(2, pcall(os.getenv, "LUAEXT_TEST_VISIBLE\0PATH"))');
probe($sandbox, 'select(2, pcall(os.getenv, "PATH=x"))');

// Nothing about the allow list is reachable as a value: it lives in an upvalue
// of a C closure, and this sandbox holds only debugTraceback -- so getupvalue
// is a gate stub, truthy but unable to run. The gate is fatal, so it escapes
// even an in-script pcall; the assertion is host-side.
try {
	// A Lua closure, not print: the upvalue guard wrapper refuses C-function
	// targets with its own catchable error before the gate is ever consulted.
	(void) $sandbox->eval(
		'local u = 1 local f = function() return u end return pcall(debug.getupvalue, f, 1)',
		'=upvalue-gate');
	echo "debug.getupvalue RAN
";
} catch (DevelopGravity\LuaExt\Exception\FeatureNotGrantedError) {
	echo "debug.getupvalue is a gate, past pcall               => true
";
}

// Changing the environment after construction changes nothing about what the
// sandbox may see -- the SET is fixed, though the VALUES are read live.
putenv('LUAEXT_TEST_HIDDEN=changed');
putenv('LUAEXT_TEST_ABSENT=now set');

probe($sandbox, 'os.getenv("LUAEXT_TEST_HIDDEN")');
probe($sandbox, 'os.getenv("LUAEXT_TEST_ABSENT")');

// Without the capability getenv is a gate stub: truthy, and unable to read a
// single variable -- the allow list never even materialises.
$without = new Sandbox(new SandboxConfig(
	capabilities: Capabilities::untrusted()->with(osEnvAllowList: ['LUAEXT_TEST_VISIBLE']),
));

try {
	(void) $without->eval('return os.getenv("LUAEXT_TEST_VISIBLE")', '=no-cap');
	echo "os.getenv RAN WITHOUT THE CAPABILITY
";
} catch (DevelopGravity\LuaExt\Exception\FeatureNotGrantedError) {
	echo "os.getenv without osEnv is a gate                    => true
";
}

?>
--EXPECT--
os.getenv("LUAEXT_TEST_VISIBLE")             => 'visible value'
os.getenv("LUAEXT_TEST_HIDDEN")              => NULL
os.getenv("LUAEXT_TEST_ABSENT")              => NULL
os.getenv("LUAEXT_TEST_NEVER_LISTED")        => NULL
os.getenv("PATH")                            => NULL
select(2, pcall(os.getenv, "LUAEXT_TEST_VISIBLE\0PATH")) => 'bad argument #1 to \'?\' (an environment variable name cannot contain \'\\0\' or \'=\')'
select(2, pcall(os.getenv, "PATH=x"))        => 'bad argument #1 to \'?\' (an environment variable name cannot contain \'\\0\' or \'=\')'
debug.getupvalue is a gate, past pcall               => true
os.getenv("LUAEXT_TEST_HIDDEN")              => NULL
os.getenv("LUAEXT_TEST_ABSENT")              => 'now set'
os.getenv without osEnv is a gate                    => true
