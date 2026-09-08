--TEST--
registerClass() refuses when the autoloader it triggered closed the sandbox
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Exception\ClosedSandboxError;
use DevelopGravity\LuaExt\Sandbox;

$sandbox = new Sandbox();

// Resolving the class name runs arbitrary PHP — an autoloader may do anything
// to the sandbox, including closing it. Registration must notice and refuse,
// never touch the interpreter it already gave up.
// eval() is how a phpt defines a class at autoload time; the code is a fixed
// literal, nothing user-supplied.
spl_autoload_register(static function (string $class) use ($sandbox): void {
	if ($class === 'VanishingHelper') {
		$sandbox->close();
		eval(<<<'PHP'
			final class VanishingHelper
			{
				#[DevelopGravity\LuaExt\LuaMethod]
				public static function ping(): int { return 1; }
			}
		PHP);
	}
});

try {
	$sandbox->registerClass('VanishingHelper');
	echo "NOT REFUSED\n";
} catch (ClosedSandboxError $error) {
	echo 'refused: ', $error->getMessage(), "\n";
}

?>
--EXPECT--
refused: The sandbox has been closed
