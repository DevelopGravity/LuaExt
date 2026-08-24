--TEST--
No protected call a script can write swallows a host failure
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Capabilities;
use DevelopGravity\LuaExt\Sandbox;
use DevelopGravity\LuaExt\SandboxConfig;

// A callback throwing something that is not a RuntimeError is a failure the
// script had no part in, so luaext_error_raise_from_exception() makes it fatal.
// From there it is exactly the same error value a limit breach uses, which is
// why this test doubles as the pcall half of the limit guarantee without needing
// the watchdog to be armed.

$attacks = [
	'pcall' => 'local ok = pcall(host.fail) return "swallowed"',
	'nested pcall' => 'pcall(function() pcall(host.fail) end) return "swallowed"',
	'pcall of a pcall' => 'pcall(pcall, host.fail) return "swallowed"',
	'in a to-be-closed' => 'do local _ <close> = setmetatable({}, {__close = function() pcall(host.fail) end}) end
		return "swallowed"',

	// load() is a protected call in disguise: lua_load runs the parser under
	// luaD_protectedparser, so an error raised by the reader -- which is script
	// code, and can call out to the host -- comes back to load()'s caller as an
	// ordinary `fail, message` return rather than as a raise.
	'in a load reader' => 'load(function() return host.fail() end) return "swallowed"',
];

$config = new SandboxConfig(capabilities: (new Capabilities())->with(compileAtRuntime: true));

foreach ($attacks as $label => $code) {
	$sandbox = new Sandbox($config);
	$sandbox->registerLibrary('host', [
		'fail' => static function (): never { throw new LogicException('the host broke'); },
	]);

	try {
		$result = $sandbox->eval($code, '=attack');
		printf("%-18s ESCAPED: %s\n", $label, var_export($result, true));
	} catch (LogicException $error) {
		printf("%-18s stopped: %s\n", $label, $error->getMessage());
	} catch (Throwable $error) {
		printf("%-18s WRONG CLASS: %s\n", $label, $error::class);
	}

	$sandbox->close();
}

// A finaliser is the one place the re-raise cannot reach the host, and the
// reason is Lua's, not ours: luaC_GCTM runs __gc under its own protected call
// and turns any error into a warning, because there is no caller to hand it to.
// What pcall still owes here is that it did not RETURN to the finaliser body,
// and that is what this pins -- a pcall that swallowed the fatal would let the
// line after it run.
$sandbox = new Sandbox();
$sandbox->registerLibrary('host', [
	'fail' => static function (): never { throw new LogicException('the host broke'); },
]);

(void) $sandbox->eval(<<<'LUA'
	resumed = false

	local doomed = setmetatable({}, {__gc = function()
		pcall(host.fail)
		resumed = true
	end})

	doomed = nil
	collectgarbage("collect")
LUA, '=finaliser');

printf("in a finaliser     pcall did not return: %s\n", var_export($sandbox->getGlobal('resumed') === false, true));

$sandbox->close();

// An ordinary script error is still the script's to catch. A sandbox where
// pcall caught nothing would be no more usable than one where it caught
// everything.
$sandbox = new Sandbox();

var_dump($sandbox->eval('local ok, err = pcall(function() error("mine") end) return ok, err', '=ordinary'));
var_dump($sandbox->eval('local ok, err = pcall(function() return nil + 1 end) return ok, err', '=ordinary'));

$sandbox->close();

?>
--EXPECT--
pcall              stopped: the host broke
nested pcall       stopped: the host broke
pcall of a pcall   stopped: the host broke
in a to-be-closed  stopped: the host broke
in a load reader   stopped: the host broke
in a finaliser     pcall did not return: true
array(2) {
  [0]=>
  bool(false)
  [1]=>
  string(16) "ordinary:1: mine"
}
array(2) {
  [0]=>
  bool(false)
  [1]=>
  string(56) "ordinary:1: attempt to perform arithmetic on a nil value"
}
