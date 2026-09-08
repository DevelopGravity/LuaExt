--TEST--
Out-of-range luaext INI values are refused, so what ini_get() reports is what runs
--EXTENSIONS--
luaext
--INI--
luaext.hook_count=4294967296
luaext.watchdog_resolution_us=5000000
--FILE--
<?php

declare(strict_types=1);

// Both handlers used to clamp silently: phpinfo() and ini_get() showed the
// configured text while the extension ran on the clamped number. Refusal (the
// same treatment negatives always got) keeps the two in agreement -- a value
// that cannot take effect never lands, and the default holds.
var_dump(ini_get('luaext.hook_count'));
var_dump(ini_get('luaext.watchdog_resolution_us'));

?>
--EXPECT--
string(4) "1000"
string(3) "500"
