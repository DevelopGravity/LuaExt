--TEST--
enableProfiler() with a period past the int ceiling arms a defined, silent hook
--EXTENSIONS--
luaext
--SKIPIF--
<?php
use DevelopGravity\LuaExt\LimitSupport;
use DevelopGravity\LuaExt\Sandbox;

if (Sandbox::features()['cpuLimit'] === LimitSupport::Unsupported) {
	echo "skip this build reports LimitSupport::Unsupported for the CPU limit";
}
?>
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Sandbox;

// period * the nominal instruction rate overflows int for any period above
// ~43 seconds, and double-to-int conversion of an out-of-range value is
// undefined behaviour -- on x86 it produced INT_MIN, a negative base count
// whose decrement never reaches zero. The clamp pins the product to INT_MAX
// first: the hook is armed, defined, and simply too coarse to fire during
// any real call -- which is exactly what a sixty-second period asks for.
$sandbox = new Sandbox();

var_dump($sandbox->enableProfiler(60.0));

(void) $sandbox->eval('local x = 0 for i = 1, 100000 do x = x + i end return x', '=coarse');

var_dump($sandbox->getProfile());

$sandbox->disableProfiler();
$sandbox->close();

?>
--EXPECT--
bool(true)
array(0) {
}
