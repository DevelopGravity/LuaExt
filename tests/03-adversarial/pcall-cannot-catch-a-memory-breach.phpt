--TEST--
pcall cannot catch the memory limit, which the error value alone would not stop
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Capabilities;
use DevelopGravity\LuaExt\Exception\FatalError;
use DevelopGravity\LuaExt\Exception\MemoryLimitError;
use DevelopGravity\LuaExt\Limits;
use DevelopGravity\LuaExt\Sandbox;
use DevelopGravity\LuaExt\SandboxConfig;

// The case a fatal-marker test on its own would miss.
//
// When the allocator refuses an allocation, Lua raises LUA_ERRMEM carrying its
// OWN preallocated string -- not the extension's unforgeable userdata -- so a
// pcall that only inspects the error value sees an ordinary catchable string and
// hands the script its memory limit back as `false, "not enough memory"`.
//
// The test therefore has to be on the lua_pcallk STATUS as well, and the status
// has to be turned back into the fatal marker rather than re-raised as a string:
// re-raising leaves the next pcall out seeing LUA_ERRRUN, and a NESTED pcall
// would catch what this one refused.

$config = new SandboxConfig(limits: new Limits(memoryBytes: 2 * 1024 * 1024));

$attacks = [
	'pcall' => 'local kept = {}
		pcall(function() while true do kept[#kept + 1] = string.rep("x", 4096) end end)
		return "swallowed"',
	'nested pcall' => 'local kept = {}
		pcall(function()
			pcall(function() while true do kept[#kept + 1] = string.rep("x", 4096) end end)
		end)
		return "swallowed"',
	'xpcall' => 'local kept = {}
		xpcall(function() while true do kept[#kept + 1] = string.rep("x", 4096) end end,
			function() return "handled" end)
		return "swallowed"',
];

// A FOURTH ROUTE, THROUGH THE PARSER RATHER THAN THE ALLOCATOR'S CALLERS.
//
// load() reports a failed parse as `fail, message` -- an ordinary return value,
// not a raise -- so a refusal that happens INSIDE the parser is the one shape
// where a memory breach could arrive as something a script may simply ignore.
// luaext_baselib.c names this as the case its status check cannot see, because
// no status survives load()'s return.
//
// It does not currently escape: the allocator's refusal unwinds as a fatal
// before load() can turn it into a return value. This pins that, so a change to
// the error path cannot quietly turn a limit breach back into a value.
//
// It needs compileAtRuntime, which the other three deliberately lack, so it runs
// against its own sandbox.
$parserAttack = 'local source = "return \"" .. string.rep("x", 3 * 1024 * 1024) .. "\""
	local ballast = string.rep("b", 3 * 1024 * 1024)
	local chunk, message = load(source)
	return "swallowed: " .. tostring(message)';

$parserSandbox = new Sandbox(new SandboxConfig(
	capabilities: (new Capabilities())->with(compileAtRuntime: true),
	limits: new Limits(memoryBytes: 8 * 1024 * 1024, maxSourceBytes: 0),
));

try {
	$result = $parserSandbox->eval($parserAttack, '=memory');
	printf("%-13s LIMIT ESCAPED: %s\n", 'load', var_export($result, true));
} catch (MemoryLimitError $error) {
	printf("%-13s stopped, and it is a FatalError: %s\n", 'load',
		var_export($error instanceof FatalError, true));
} catch (Throwable $error) {
	printf("%-13s WRONG CLASS: %s\n", 'load', $error::class);
}

$parserSandbox->close();

foreach ($attacks as $label => $code) {
	$sandbox = new Sandbox($config);

	try {
		$result = $sandbox->eval($code, '=memory');
		printf("%-13s LIMIT ESCAPED: %s\n", $label, var_export($result, true));
	} catch (MemoryLimitError $error) {
		printf("%-13s stopped, and it is a FatalError: %s\n", $label,
			var_export($error instanceof FatalError, true));
	} catch (Throwable $error) {
		printf("%-13s WRONG CLASS: %s\n", $label, $error::class);
	}

	$sandbox->close();
}

?>
--EXPECT--
load          stopped, and it is a FatalError: true
pcall         stopped, and it is a FatalError: true
nested pcall  stopped, and it is a FatalError: true
xpcall        stopped, and it is a FatalError: true
