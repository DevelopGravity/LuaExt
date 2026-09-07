--TEST--
luaext.hook_count refuses a negative interval and cannot be moved from userland
--EXTENSIONS--
luaext
--INI--
luaext.hook_count=-5
--FILE--
<?php

declare(strict_types=1);

// The raw OnUpdateLong accepted any zend_long and let the consumer's (int)
// cast truncate it -- 4294967296 became a base count of 0, a hook that could
// never fire, on the one build where that hook IS the CPU limit. The custom
// handler refuses negatives (so the -5 above never landed and the default
// held) and clamps past INT_MAX; PHP_INI_SYSTEM keeps userland's hands off a
// knob that gates an enforcement mechanism.
var_dump(ini_get('luaext.hook_count'));

var_dump(@ini_set('luaext.hook_count', '500'));
var_dump(ini_get('luaext.hook_count'));

?>
--EXPECT--
string(4) "1000"
bool(false)
string(4) "1000"
