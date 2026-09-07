--TEST--
wrapCallable() and getProfile() enforce the parameter types the stubs publish
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Sandbox;

// For an internal function ZPP is the only thing that enforces parameter
// types -- the arginfo the stubs generate is never consulted at call time.
// wrapCallable parsed its `callable` as a bare zval and refused later with a
// ConfigurationError (a LogicException), and getProfile's ZPP accepted the
// null its declared non-nullable ProfilerUnit forbids. Both now fail the way
// the published signatures promise: with the engine's TypeError, naming the
// argument.
$sandbox = new Sandbox();

try {
	$sandbox->wrapCallable(42);
	echo "wrapCallable(42): NOT REFUSED\n";
} catch (TypeError $error) {
	printf("wrapCallable(42): TypeError: %s\n", $error->getMessage());
}

try {
	(void) $sandbox->getProfile(null);
	echo "getProfile(null): NOT REFUSED\n";
} catch (TypeError $error) {
	printf("getProfile(null): TypeError: %s\n", $error->getMessage());
}

// The happy paths are untouched: a real callable wraps and calls, and the
// default unit still answers when the argument is simply omitted.
$reverse = $sandbox->wrapCallable(strrev(...), 'reverse');

var_dump($reverse->call('desrever'));
var_dump($sandbox->getProfile());

$sandbox->close();

?>
--EXPECT--
wrapCallable(42): TypeError: DevelopGravity\LuaExt\Sandbox::wrapCallable(): Argument #1 ($callback) must be a valid callback, no array or string given
getProfile(null): TypeError: DevelopGravity\LuaExt\Sandbox::getProfile(): Argument #1 ($unit) must be of type DevelopGravity\LuaExt\ProfilerUnit, null given
array(1) {
  [0]=>
  string(8) "reversed"
}
array(0) {
}
