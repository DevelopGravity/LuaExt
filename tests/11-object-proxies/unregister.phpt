--TEST--
unregister() releases a claimed name so it can be swapped, retiring a class cleanly
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Exception\ConfigurationError;
use DevelopGravity\LuaExt\Exception\ConversionError;
use DevelopGravity\LuaExt\LuaMethod;
use DevelopGravity\LuaExt\Sandbox;

final class Clock
{
	#[LuaMethod]
	public static function now(): int { return 42; }
}

final class Pinger
{
	#[LuaMethod]
	public function ping(): int { return 1; }
}

$sandbox = new Sandbox();

// Swap: a library gives way to a class under the same name.
$sandbox->registerLibrary('svc', ['ping' => static fn (): int => 7]);
$sandbox->unregister('svc');
var_dump($sandbox->eval('return type(svc)')[0]);
$sandbox->registerClass(Clock::class, luaName: 'svc');
var_dump($sandbox->eval('return svc.now()')[0]);

// Retiring a class: proxies a script already holds keep working — an object a
// script was given cannot be taken back — but NEW instances stop wrapping and
// the name is free again.
$sandbox->registerClass(Pinger::class);
$sandbox->setGlobal('p', new Pinger());
$sandbox->unregister('Pinger');
var_dump($sandbox->eval('return p:ping()')[0]);

try {
	$sandbox->setGlobal('q', new Pinger());
	echo "NOT REFUSED\n";
} catch (ConversionError) {
	echo "retired class refused\n";
}

$sandbox->registerLibrary('Pinger', ['free' => static fn (): bool => true]);
var_dump($sandbox->eval('return Pinger.free()')[0]);

// The same class may register again after retirement, under a fresh name.
$sandbox->registerClass(Pinger::class, luaName: 'PingerAgain');
$sandbox->setGlobal('r', new Pinger());
var_dump($sandbox->eval('return r:ping()')[0]);

// Only claimed names can be released; free-form globals are setGlobal()'s job.
try {
	$sandbox->unregister('ghost');
} catch (ConfigurationError $error) {
	echo $error->getMessage(), "\n";
}

$sandbox->close();

?>
--EXPECT--
string(3) "nil"
int(42)
int(1)
retired class refused
bool(true)
int(1)
Nothing is registered under the Lua name "ghost"
