--TEST--
A ModuleResolver crossing counts in phpCallsOut, in step with the time it is billed
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Capabilities;
use DevelopGravity\LuaExt\ModuleResolver;
use DevelopGravity\LuaExt\ModuleSource;
use DevelopGravity\LuaExt\Sandbox;
use DevelopGravity\LuaExt\SandboxConfig;

// The stub documents phpWallClockSeconds as covering "registered callables,
// the output callback, and the module resolver", and phpCallsOut as "calls
// from Lua back into PHP". The resolver crossing was timed into the first
// bucket but never counted in the second, so stats reported host time with
// no call to attribute it to.
final class CountedResolver implements ModuleResolver
{
	public int $resolved = 0;

	public function resolve(string $module, string $requestedBy): ?ModuleSource
	{
		$this->resolved++;

		return new ModuleSource(sprintf('return { name = %s }', var_export($module, true)),
			'@resolver/' . $module);
	}
}

$resolver = new CountedResolver();

$sandbox = new Sandbox(new SandboxConfig(
	capabilities: (new Capabilities())->with(require: true),
	moduleResolver: $resolver,
));

$before = $sandbox->stats()->phpCallsOut;

[$name] = $sandbox->eval('return require("counted").name', '=resolved');

$after = $sandbox->stats()->phpCallsOut;

var_dump($name, $resolver->resolved, $after - $before);

// A cached re-require crosses nothing and counts nothing.
(void) $sandbox->eval('return require("counted").name', '=cached');

var_dump($sandbox->stats()->phpCallsOut - $after);

$sandbox->close();

?>
--EXPECT--
string(7) "counted"
int(1)
int(1)
int(0)
