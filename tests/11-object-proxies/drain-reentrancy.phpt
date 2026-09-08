--TEST--
Destructors released by the drain may re-enter the sandbox, and may close it
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\LuaMethod;
use DevelopGravity\LuaExt\Sandbox;

final class Reenters
{
	public static ?Sandbox $sandbox = null;
	public static string $seen = '';

	#[LuaMethod]
	public function id(): int { return 1; }

	public function __destruct()
	{
		self::$seen = self::$sandbox?->eval('return "re-entered"')[0] ?? 'no sandbox';
	}
}

$sandbox = new Sandbox();
Reenters::$sandbox = $sandbox;
$sandbox->registerClass(Reenters::class);
$sandbox->setGlobal('r', new Reenters());
(void) $sandbox->eval('r = nil collectgarbage("collect")');
var_dump(Reenters::$seen);

// And a destructor that closes the sandbox mid-drain leaves it closed, not corrupt.
final class Closes
{
	public static ?Sandbox $sandbox = null;

	#[LuaMethod]
	public function id(): int { return 1; }

	public function __destruct()
	{
		self::$sandbox?->close();
	}
}

$second = new Sandbox();
Closes::$sandbox = $second;
$second->registerClass(Closes::class);
$second->setGlobal('c', new Closes());
(void) $second->eval('c = nil collectgarbage("collect")');
var_dump($second->isClosed());

?>
--EXPECT--
string(10) "re-entered"
bool(true)
