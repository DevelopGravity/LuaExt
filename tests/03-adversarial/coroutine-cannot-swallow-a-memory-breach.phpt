--TEST--
No coroutine construct can swallow a memory breach, which the error value alone would not stop
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Exception\FatalError;
use DevelopGravity\LuaExt\Exception\MemoryLimitError;
use DevelopGravity\LuaExt\Limits;
use DevelopGravity\LuaExt\Sandbox;
use DevelopGravity\LuaExt\SandboxConfig;

// The coroutine twin of pcall-cannot-catch-a-memory-breach.phpt, and it is a
// SEPARATE trap rather than the same one reached by another route.
//
// resume is a pcall in disguise, so it has the same obligation to re-raise a
// fatal instead of reporting it. Every other fatal arrives as the extension's
// own unforgeable userdata, so a resume wrapper that inspected the error VALUE
// would appear to work -- against a CPU limit, a wall-clock limit, an output
// limit, all of them.
//
// LUA_ERRMEM is the one that gets through. When the allocator refuses, Lua
// raises it carrying its own preallocated "not enough memory" string, because
// building our userdata would itself need an allocation. A value-inspecting
// wrapper sees an ordinary catchable string, and a script that moved its
// allocation into a coroutine gets its memory limit handed back as
// `false, "not enough memory"` -- then loops and does it again.
//
// So the check has to key on the STATUS from lua_resume, and LUA_ERRMEM has to
// be named in it explicitly rather than covered by a fatal-marker test.

$config = new SandboxConfig(limits: new Limits(memoryBytes: 2 * 1024 * 1024));

$burn = 'while true do kept[#kept + 1] = string.rep("x", 4096) end';

$attacks = [
	// The direct route: resume reports the breach, the script ignores it.
	'resume' => "local kept = {}
		local co = coroutine.create(function() $burn end)
		coroutine.resume(co)
		return 'swallowed'",

	// wrap re-raises into the caller, so a pcall around it is the natural
	// second attempt -- and it must not help either.
	'wrap in pcall' => "local kept = {}
		pcall(coroutine.wrap(function() $burn end))
		return 'swallowed'",

	// Nesting is where a status check that re-raises as a plain string breaks:
	// the outer resume would see LUA_ERRRUN and catch what the inner refused.
	'nested resume' => "local kept = {}
		local inner = coroutine.create(function() $burn end)
		local outer = coroutine.create(function() coroutine.resume(inner) end)
		coroutine.resume(outer)
		return 'swallowed'",

	// The handler must not run for a fatal, exactly as xpcall's does not.
	'resume in xpcall' => "local kept = {}
		xpcall(function()
			local co = coroutine.create(function() $burn end)
			coroutine.resume(co)
		end, function() return 'handled' end)
		return 'swallowed'",
];

foreach ($attacks as $label => $code) {
	$sandbox = new Sandbox($config);

	try {
		$result = $sandbox->eval($code, '=memory');
		printf("%-17s LIMIT ESCAPED: %s\n", $label, var_export($result, true));
	} catch (MemoryLimitError $error) {
		printf("%-17s stopped, and it is a FatalError: %s\n", $label,
			var_export($error instanceof FatalError, true));
	} catch (Throwable $error) {
		printf("%-17s WRONG CLASS: %s\n", $label, $error::class);
	}

	$sandbox->close();
}

?>
--EXPECT--
resume            stopped, and it is a FatalError: true
wrap in pcall     stopped, and it is a FatalError: true
nested resume     stopped, and it is a FatalError: true
resume in xpcall  stopped, and it is a FatalError: true
