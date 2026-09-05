--TEST--
No protected call a script can write turns a withheld-feature touch into data
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Capabilities;
use DevelopGravity\LuaExt\Exception\FeatureNotGrantedError;
use DevelopGravity\LuaExt\Sandbox;
use DevelopGravity\LuaExt\SandboxConfig;

// FeatureNotGrantedError is fatal by decision: a script must not discover
// policy by probing calls, so the error survives everything the limit errors
// survive. The supported in-script probe is truthiness on tier-1 libraries
// (`if coroutine then`), which never touches the nil; everything below tries
// to launder the fatal into a catchable value and must fail.

$sandbox = new Sandbox(new SandboxConfig(
	capabilities: Capabilities::untrusted()->with(coroutines: true), // coroutines for resume routes
));

$routes = [
	'pcall gate' => 'return pcall(os.getenv, "PATH")',
	'pcall tier1' => 'return pcall(function() return require("m") end)',
	'nested pcall' => 'return pcall(pcall, os.getenv, "PATH")',
	'xpcall handler' => 'return xpcall(os.getenv, function() return "laundered" end, "PATH")',
	'resume' => 'local co = coroutine.create(function() return os.getenv("PATH") end) return coroutine.resume(co)',
	'wrap' => 'return coroutine.wrap(function() return os.getenv("PATH") end)()',
	'close handler' => 'do local x <close> = setmetatable({}, {__close = function() os.getenv("PATH") end}) end return "survived"',
];

foreach ($routes as $label => $script) {
	try {
		(void) $sandbox->eval($script, '=launder');
		printf("%-14s => LAUNDERED\n", $label);
	} catch (FeatureNotGrantedError) {
		printf("%-14s => fatal survived\n", $label);
	}
}

$sandbox->close();

?>
--EXPECT--
pcall gate     => fatal survived
pcall tier1    => fatal survived
nested pcall   => fatal survived
xpcall handler => fatal survived
resume         => fatal survived
wrap           => fatal survived
close handler  => fatal survived
