--TEST--
The default Capabilities are exactly the untrusted baseline
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Capabilities;

// The constructor defaults ARE the untrusted baseline: there is no separate
// "safe" preset to remember to reach for.
$default = new Capabilities();
$preset = Capabilities::untrusted();

$flags = [];

foreach ((new ReflectionClass(Capabilities::class))->getProperties() as $property) {
	$name = $property->getName();

	if ($name === 'osEnvAllowList') {
		continue;
	}

	$flags[$name] = $default->$name;

	if ($default->$name !== $preset->$name) {
		printf("MISMATCH between new Capabilities() and untrusted() on %s\n", $name);
	}
}

// Everything that grants reach outside the interpreter is off; what is left on
// is in-process language surface a script cannot escape through.
var_dump(array_keys(array_filter($flags)));
var_dump(array_keys(array_filter($flags, static fn (bool $on): bool => !$on)));

var_dump($default->osEnvAllowList);

// Two calls are two objects, never a shared singleton a host could smuggle
// state through.
var_dump(Capabilities::untrusted() !== Capabilities::untrusted());

?>
--EXPECT--
array(4) {
  [0]=>
  string(10) "coroutines"
  [1]=>
  string(6) "osTime"
  [2]=>
  string(14) "debugTraceback"
  [3]=>
  string(4) "utf8"
}
array(12) {
  [0]=>
  string(12) "loadBytecode"
  [1]=>
  string(16) "compileAtRuntime"
  [2]=>
  string(12) "dumpBytecode"
  [3]=>
  string(7) "require"
  [4]=>
  string(3) "vfs"
  [5]=>
  string(8) "vfsWrite"
  [6]=>
  string(5) "osEnv"
  [7]=>
  string(15) "debugIntrospect"
  [8]=>
  string(11) "debugMutate"
  [9]=>
  string(10) "debugHooks"
  [10]=>
  string(9) "gcControl"
  [11]=>
  string(4) "warn"
}
array(0) {
}
bool(true)
