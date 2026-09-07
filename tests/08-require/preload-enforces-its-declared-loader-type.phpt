--TEST--
preloadModule() raises the engine's TypeError for a loader outside its declared union
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Capabilities;
use DevelopGravity\LuaExt\Sandbox;
use DevelopGravity\LuaExt\SandboxConfig;

// The stub declares `LuaFunction|callable $loader`, but no single ZPP macro
// spells that union, so for internal functions nothing enforces it unless the
// method does. A wrong-typed argument must surface as the engine's TypeError
// naming the argument position -- the contract every other declared parameter
// keeps -- not as a ConfigurationError from deep inside loader resolution.

$sandbox = new Sandbox(new SandboxConfig(
	capabilities: (new Capabilities())->with(require: true),
));

foreach ([
	'an integer' => 42,
	'a non-callable string' => 'no_such_function_anywhere',
	'an array of nonsense' => [1, 2, 3],
	'a plain object' => new stdClass(),
] as $label => $loader) {
	try {
		$sandbox->preloadModule('m', $loader);
		printf("%-22s ACCEPTED\n", $label);
	} catch (TypeError $error) {
		printf("%-22s TypeError: %s\n", $label, $error->getMessage());
	}
}

// Both halves of the union still work.
$sandbox->preloadModule('from_callable', static fn (): string => 'callable ok');
$sandbox->preloadModule('from_handle', $sandbox->compile('return function() return "handle ok" end', '@h')->call()[0]);
printf("%s / %s\n", ...$sandbox->eval('return require("from_callable"), require("from_handle")', '=r'));

$sandbox->close();

?>
--EXPECT--
an integer             TypeError: DevelopGravity\LuaExt\Sandbox::preloadModule(): Argument #2 ($loader) must be of type DevelopGravity\LuaExt\LuaFunction|callable, int given
a non-callable string  TypeError: DevelopGravity\LuaExt\Sandbox::preloadModule(): Argument #2 ($loader) must be of type DevelopGravity\LuaExt\LuaFunction|callable, string given
an array of nonsense   TypeError: DevelopGravity\LuaExt\Sandbox::preloadModule(): Argument #2 ($loader) must be of type DevelopGravity\LuaExt\LuaFunction|callable, array given
a plain object         TypeError: DevelopGravity\LuaExt\Sandbox::preloadModule(): Argument #2 ($loader) must be of type DevelopGravity\LuaExt\LuaFunction|callable, stdClass given
callable ok / handle ok
