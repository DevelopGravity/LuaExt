--TEST--
Registered instances wrap at every PHP-to-Lua crossing and unwrap to the original instance
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Exception\ConversionError;
use DevelopGravity\LuaExt\LuaMethod;
use DevelopGravity\LuaExt\Sandbox;

class Ticket
{
	#[LuaMethod]
	public function id(): int { return 7; }
}

final class Unregistered {}

$sandbox = new Sandbox();
$sandbox->registerClass(Ticket::class);

$ticket = new Ticket();

// Crossing 1: setGlobal.
$sandbox->setGlobal('viaGlobal', $ticket);
var_dump($sandbox->eval('return type(viaGlobal)')[0]);

// Crossing 2: a registered callable's return value.
$sandbox->registerLibrary('host', ['ticket' => static fn (): Ticket => $ticket]);
var_dump($sandbox->eval('return type(host.ticket())')[0]);

// Crossing 3: an argument to call().
(void) $sandbox->eval('function probe(value) return type(value) end');
var_dump($sandbox->call('probe', $ticket)[0]);

// Round trip: the host gets the ORIGINAL instance back, all three ways.
var_dump($sandbox->getGlobal('viaGlobal') === $ticket);
var_dump($sandbox->eval('return viaGlobal')[0] === $ticket);
$sandbox->registerLibrary('sink', ['take' => static function (Ticket $got) use ($ticket): bool {
	return $got === $ticket;
}]);
var_dump($sandbox->eval('return sink.take(viaGlobal)')[0]);

// A subclass instance wraps as its nearest registered ancestor.
$sub = new class extends Ticket {};
$sandbox->setGlobal('viaSub', $sub);
var_dump($sandbox->eval('return type(viaSub)')[0]);
var_dump($sandbox->getGlobal('viaSub') === $sub);

// Unregistered classes keep today's refusal, at every crossing.
try {
	$sandbox->setGlobal('nope', new Unregistered());
} catch (ConversionError $error) {
	echo "refused: ", str_contains($error->getMessage(), 'registerClass') ? "mentions registerClass\n" : "MISSING HINT\n";
}

$sandbox->close();

?>
--EXPECT--
string(8) "userdata"
string(8) "userdata"
string(8) "userdata"
bool(true)
bool(true)
bool(true)
string(8) "userdata"
bool(true)
refused: mentions registerClass
