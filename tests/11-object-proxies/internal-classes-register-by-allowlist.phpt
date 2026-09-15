--TEST--
An engine-defined class registers through an explicit allowlist and dispatches
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Exception\ConfigurationError;
use DevelopGravity\LuaExt\Sandbox;

// Registration gates on what a class IS -- interface, trait, enum, anonymous --
// never on who defined it, so an internal class is registrable like any other.
// It can carry no attributes, which makes the allowlist the only route in.
$sandbox = new Sandbox();
$sandbox->registerClass(\ArrayObject::class, methods: ['count', 'getArrayCopy'], luaName: 'bag');

$bag = new \ArrayObject([1, 2, 3]);
$sandbox->setGlobal('b', $bag);

// Instances cross as proxies and dispatch to the engine's own implementation.
var_dump($sandbox->eval('return b:count()')[0]);
var_dump($sandbox->eval('return type(b)')[0]);

// A returned array converts by the ordinary rules. Summed with pairs() so the
// assertion is about the values crossing, not about which end the keys start.
var_dump($sandbox->eval('local s = 0 for _, v in pairs(b:getArrayCopy()) do s = s + v end return s')[0]);

// Nothing outside the allowlist is reachable, however public it is upstream.
var_dump($sandbox->eval('return b.append == nil')[0]);
var_dump($sandbox->eval('local ok = pcall(function() return b:append(4) end) return ok')[0]);

// The host's object is the same one throughout.
$sandbox->registerLibrary('host', ['take' => static function (\ArrayObject $given) use ($bag): bool {
	return $given === $bag;
}]);
var_dump($sandbox->eval('return host.take(b)')[0]);

// No constructor is exposed, so no class table is planted -- but the name is
// claimed all the same, and claiming it twice is refused.
try {
	$sandbox->registerClass(\ArrayIterator::class, methods: ['count'], luaName: 'bag');
	echo "NOT REFUSED\n";
} catch (ConfigurationError $error) {
	echo 'refused: ', $error->getMessage(), "\n";
}

$sandbox->close();

?>
--EXPECTF--
int(3)
string(8) "userdata"
int(6)
bool(true)
bool(false)
bool(true)
refused: %s
