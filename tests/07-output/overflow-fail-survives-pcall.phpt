--TEST--
A script cannot pcall its way past its own output budget
--EXTENSIONS--
luaext
--XFAIL--
Needs two things from the library-policy wave: print(), so a script can reach luaext_output_write() at all, and the pcall replacement, because Lua's own pcall is still upstream's and will swallow the fatal this test says it must not. The sink already reports the breach as fatal; nothing yet stops pcall catching it.
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Exception\OutputLimitError;
use DevelopGravity\LuaExt\Limits;
use DevelopGravity\LuaExt\Sandbox;
use DevelopGravity\LuaExt\SandboxConfig;

// A limit a script can catch is not a limit. This is the property the whole
// Fail behaviour exists for: overflowing raises an unforgeable fatal error, and
// neither pcall nor xpcall gets a say in whether it propagates.

$sandbox = new Sandbox(new SandboxConfig(limits: new Limits(outputBytes: 8)));

try {
	(void) $sandbox->eval(<<<'LUA'
		local ok, err = pcall(function()
			print("this is well past eight bytes")
		end)

		return "pcall returned " .. tostring(ok)
	LUA, '=budget');
	echo "PCALL SWALLOWED THE OUTPUT LIMIT\n";
} catch (OutputLimitError $error) {
	printf("propagated out of pcall: %s\n", $error::class);
}

$sandbox->close();

// xpcall is the sharper version of the same hole: a message handler's return
// value becomes the error object, so one line of Lua could replace the marker
// with a plain string every outer pcall would then treat as catchable.
$second = new Sandbox(new SandboxConfig(limits: new Limits(outputBytes: 8)));

try {
	(void) $second->eval(<<<'LUA'
		local ok = xpcall(function()
			print("this is well past eight bytes")
		end, function() return "harmless string" end)

		return "xpcall returned " .. tostring(ok)
	LUA, '=budget');
	echo "XPCALL LAUNDERED THE OUTPUT LIMIT\n";
} catch (OutputLimitError $error) {
	printf("propagated out of xpcall: %s\n", $error::class);
}

$second->close();

?>
--EXPECT--
propagated out of pcall: DevelopGravity\LuaExt\Exception\OutputLimitError
propagated out of xpcall: DevelopGravity\LuaExt\Exception\OutputLimitError
