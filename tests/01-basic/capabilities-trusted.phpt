--TEST--
Capabilities::trusted() widens the documented flags and no others
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Capabilities;

$untrusted = Capabilities::untrusted();
$trusted = Capabilities::trusted();

$widened = [];

foreach ((new ReflectionClass(Capabilities::class))->getProperties() as $property) {
	$name = $property->getName();

	if ($name === 'osEnvAllowList') {
		continue;
	}

	if ($trusted->$name !== $untrusted->$name) {
		$widened[] = $name;
	}
}

var_dump($widened);

// The three that stay off even here. Each voids a guarantee the sandbox
// otherwise makes, so "I trust this code" is not enough to turn them on:
// they have to be asked for one at a time.
var_dump($trusted->loadBytecode, $trusted->debugMutate, $trusted->debugHooks);

// And asking is possible -- deliberately, one flag at a time.
var_dump($trusted->with(debugMutate: true)->debugMutate);

?>
--EXPECT--
array(7) {
  [0]=>
  string(16) "compileAtRuntime"
  [1]=>
  string(12) "dumpBytecode"
  [2]=>
  string(7) "require"
  [3]=>
  string(3) "vfs"
  [4]=>
  string(15) "debugIntrospect"
  [5]=>
  string(9) "gcControl"
  [6]=>
  string(4) "warn"
}
bool(false)
bool(false)
bool(false)
bool(true)
