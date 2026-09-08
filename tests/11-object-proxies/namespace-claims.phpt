--TEST--
Registrations own their names: any later registration wanting one is refused
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Exception\ConfigurationError;
use DevelopGravity\LuaExt\LuaMethod;
use DevelopGravity\LuaExt\Operator;
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
$sandbox->registerLibrary('clock', ['now' => static fn (): int => 99]);

$claims = [
	'library-library' => static function () use ($sandbox): void {
		$sandbox->registerLibrary('clock', ['now' => static fn (): int => 1]);
	},
	'class-library' => static function () use ($sandbox): void {
		$sandbox->registerClass(Clock::class, luaName: 'clock');
	},
	'object-library' => static function () use ($sandbox): void {
		$sandbox->registerObject('clock', new Pinger());
	},
];

foreach ($claims as $label => $attempt) {
	try {
		$attempt();
		echo $label, ": NOT REFUSED\n";
	} catch (ConfigurationError $error) {
		echo $label, ': ', $error->getMessage(), "\n";
	}
}

// The claim works in the other direction too.
$sandbox->registerClass(Clock::class);
try {
	$sandbox->registerLibrary('Clock', ['x' => static fn (): int => 1]);
} catch (ConfigurationError $error) {
	echo 'library-class: ', $error->getMessage(), "\n";
}

// A class exposing only instance methods plants no table but still reserves
// its name -- the registration owns it either way.
$sandbox->registerClass(Pinger::class);
try {
	$sandbox->registerLibrary('Pinger', ['x' => static fn (): int => 1]);
} catch (ConfigurationError $error) {
	echo 'library-quiet: ', $error->getMessage(), "\n";
}

// A FAILED registration burns nothing: the refused attempt below never claims
// "free", so the later library registration under that name succeeds.
try {
	$sandbox->registerClass(Clock::class, luaName: 'free', operators: ['nothere' => Operator::LessThan]);
} catch (ConfigurationError) {
}
$sandbox->registerLibrary('free', ['ok' => static fn (): bool => true]);
echo 'failed-claims-nothing: ', $sandbox->eval('return free.ok()')[0] ? "true\n" : "false\n";

// setGlobal() remains the deliberate free-form write; it never consults the
// claim table, so a host can still overwrite anything on purpose.
$sandbox->setGlobal('clock', 'shadowed');
echo 'setglobal-still-writes: ', $sandbox->eval('return clock')[0], "\n";

$sandbox->close();

?>
--EXPECT--
library-library: The Lua name "clock" is already taken by an earlier registration on this sandbox
class-library: The Lua name "clock" is already taken by an earlier registration on this sandbox
object-library: The Lua name "clock" is already taken by an earlier registration on this sandbox
library-class: The Lua name "Clock" is already taken by an earlier registration on this sandbox
library-quiet: The Lua name "Pinger" is already taken by an earlier registration on this sandbox
failed-claims-nothing: true
setglobal-still-writes: shadowed
