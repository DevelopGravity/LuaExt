--TEST--
No Lua construct can swallow a CPU-limit breach
--EXTENSIONS--
luaext
--XFAIL--
Needs the watchdog: Sandbox::setCpuLimit() still throws "not implemented yet", so nothing here reaches the point of being caught. The pcall and xpcall replacements have landed and are covered by pcall-cannot-swallow-a-host-failure.phpt and xpcall-handler-skipped-for-fatal.phpt; the coroutine.resume and coroutine.wrap cases additionally need the coroutine wrapper, which no library installs yet.
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Exception\CpuLimitError;
use DevelopGravity\LuaExt\Sandbox;

// The whole extension rests on this. If any of these constructs can catch a
// limit breach and carry on, then setCpuLimit() is a suggestion and untrusted
// code runs for as long as it likes. Every one of them is a protected call in
// disguise, and every one of them has to re-raise instead of returning.

$attacks = [
	'pcall' => 'pcall(function() while true do end end) return "swallowed"',
	'nested pcall' => 'pcall(function() pcall(function() while true do end end) end) return "swallowed"',
	'xpcall' => 'xpcall(function() while true do end end, function() return "handled" end) return "swallowed"',
	'coroutine.resume' => 'local c = coroutine.create(function() while true do end end)
		coroutine.resume(c) return "swallowed"',
	'coroutine.wrap' => 'local w = coroutine.wrap(function() while true do end end)
		pcall(w) return "swallowed"',
	'in a finaliser' => 'local t = setmetatable({}, {__gc = function() while true do end end})
		t = nil collectgarbage("collect") return "swallowed"',
	'in a to-be-closed' => 'do local _ <close> = setmetatable({}, {__close = function() while true do end end}) end
		return "swallowed"',
];

foreach ($attacks as $label => $code) {
	// A fresh sandbox each time: a spent CPU budget would trip the next attack
	// before it ever ran, which would prove nothing.
	$sandbox = new Sandbox();
	$sandbox->setCpuLimit(0.05);

	try {
		$result = $sandbox->eval($code, '=attack');
		printf("%-20s LIMIT ESCAPED: %s\n", $label, var_export($result, true));
	} catch (CpuLimitError $error) {
		printf("%-20s stopped\n", $label);
	} catch (Throwable $error) {
		printf("%-20s WRONG CLASS: %s\n", $label, $error::class);
	}

	$sandbox->close();
}

?>
--EXPECT--
pcall                stopped
nested pcall         stopped
xpcall               stopped
coroutine.resume     stopped
coroutine.wrap       stopped
in a finaliser       stopped
in a to-be-closed    stopped
