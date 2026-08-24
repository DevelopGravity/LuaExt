--TEST--
A C function's upvalues are host state and the debug library refuses to touch them
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Capabilities;
use DevelopGravity\LuaExt\Limits;
use DevelopGravity\LuaExt\Sandbox;
use DevelopGravity\LuaExt\SandboxConfig;

// This is a concrete escape, not a formality. The closure that exposes a PHP
// callable to Lua carries its zend_fcall_info_cache as upvalue 1. An unguarded
// debug.getupvalue would hand a script the host-callable storage of every
// registered function; an unguarded debug.setupvalue would let it swap one host
// function's callable into another's closure. math.random's generator state is
// covered by the same refusal.

$capabilities = Capabilities::untrusted()->with(debugIntrospect: true, debugMutate: true);

$sandbox = new Sandbox(new SandboxConfig(
	capabilities: $capabilities,
	limits: new Limits(cpuSeconds: null),
));

$sandbox->registerLibrary('host', [
	'identify' => static fn (): string => 'the real host function',
]);

/** Run `$expression` and print what came back. */
function probe(Sandbox $sandbox, string $label, string $expression): void
{
	printf("%-46s => %s\n", $label, var_export($sandbox->eval("return $expression", '=probe')[0], true));
}

// Reading.
probe($sandbox, 'getupvalue(host fn)',
	'select(2, pcall(debug.getupvalue, host.identify, 1))');

// Writing.
probe($sandbox, 'setupvalue(host fn)',
	'select(2, pcall(debug.setupvalue, host.identify, 1, "hijacked"))');

// The raw upvalue address, and the operation that rewires two closures to share
// one. Both refuse before they ever reach the C closure's storage.
probe($sandbox, 'upvalueid(host fn)',
	'select(2, pcall(debug.upvalueid, host.identify, 1))');
probe($sandbox, 'upvaluejoin(host fn, host fn)',
	'select(2, pcall(debug.upvaluejoin, host.identify, 1, host.identify, 1))');

// The same refusal covers any C function, including the ones the standard
// library is built from.
probe($sandbox, 'getupvalue(math.random)',
	'select(2, pcall(debug.getupvalue, math.random, 1))');
probe($sandbox, 'upvaluejoin(lua fn, host fn)',
	'select(2, pcall(debug.upvaluejoin, function() end, 1, host.identify, 1))');

// A refused call must leave the host function exactly as it was. A guard that
// half-applies its argument would be worse than no guard.
probe($sandbox, 'host function still works', 'host.identify()');

// And the members are otherwise upstream's, unchanged: a Lua closure's upvalues
// are script state and stay reachable, or debugIntrospect would mean nothing.
probe($sandbox, 'getupvalue(lua closure)', <<<'LUA'
	(function()
		local captured = 41
		local closure = function() return captured end
		local name, value = debug.getupvalue(closure, 1)
		return name .. "=" .. tostring(value)
	end)()
	LUA);

probe($sandbox, 'setupvalue(lua closure)', <<<'LUA'
	(function()
		local captured = 41
		local closure = function() return captured end
		debug.setupvalue(closure, 1, 42)
		return closure()
	end)()
	LUA);

?>
--EXPECT--
getupvalue(host fn)                            => 'debug.getupvalue: a C function\'s upvalues are host state, not script state'
setupvalue(host fn)                            => 'debug.setupvalue: a C function\'s upvalues are host state, not script state'
upvalueid(host fn)                             => 'debug.upvalueid: a C function\'s upvalues are host state, not script state'
upvaluejoin(host fn, host fn)                  => 'debug.upvaluejoin: a C function\'s upvalues are host state, not script state'
getupvalue(math.random)                        => 'debug.getupvalue: a C function\'s upvalues are host state, not script state'
upvaluejoin(lua fn, host fn)                   => 'debug.upvaluejoin: a C function\'s upvalues are host state, not script state'
host function still works                      => 'the real host function'
getupvalue(lua closure)                        => 'captured=41'
setupvalue(lua closure)                        => 42
