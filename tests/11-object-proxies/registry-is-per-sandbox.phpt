--TEST--
Name claims and class registries are per sandbox: retiring in one never touches another
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Exception\ConversionError;
use DevelopGravity\LuaExt\LuaMethod;
use DevelopGravity\LuaExt\Sandbox;

final class Meter
{
	#[LuaMethod]
	public function __construct(public readonly int $value = 0) {}

	#[LuaMethod]
	public function get(): int { return $this->value; }

	#[LuaMethod]
	public static function unit(): string { return 'm'; }
}

// The same class, under the same Lua name, on two independent sandboxes --
// the core "registration is per sandbox" invariant from luaext_proxy.h.
$first = new Sandbox();
$second = new Sandbox();
$first->registerClass(Meter::class);
$second->registerClass(Meter::class);

$first->setGlobal('a', new Meter(1));
$second->setGlobal('b', new Meter(2));

// Retire in the FIRST sandbox only.
$first->unregister('Meter');

// The first behaves retired: new instances refuse, the class table is gone.
try {
	$first->setGlobal('c', new Meter(3));
	echo "NOT REFUSED\n";
} catch (ConversionError) {
	echo "first refuses new instances\n";
}
var_dump($first->eval('return type(Meter)')[0]);

// The second is completely unaffected: registration, statics, live proxies,
// new instances, and its live-proxy count.
var_dump($second->eval('return Meter.unit(), Meter.new(7):get(), b:get()'));
$second->setGlobal('c', new Meter(3));
var_dump($second->eval('return c:get()')[0]);
var_dump($second->stats()->liveObjectProxies);

// And the name freed in the first is claimable there without disturbing the
// second's claim.
$first->registerLibrary('Meter', ['free' => static fn (): bool => true]);
var_dump($first->eval('return Meter.free()')[0]);
var_dump($second->eval('return Meter.unit()')[0]);

$first->close();
$second->close();

?>
--EXPECT--
first refuses new instances
string(3) "nil"
array(3) {
  [0]=>
  string(1) "m"
  [1]=>
  int(7)
  [2]=>
  int(2)
}
int(3)
int(3)
bool(true)
string(1) "m"
