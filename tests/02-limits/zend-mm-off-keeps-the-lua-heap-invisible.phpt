--TEST--
The default luaext.use_zend_mm=0 keeps the Lua heap off PHP's allocator entirely
--EXTENSIONS--
luaext
--SKIPIF--
<?php
// Same reasoning as the =1 sibling: with USE_ZEND_ALLOC=0 ZendMM is a malloc
// pass-through and memory_get_usage() measures nothing, so "invisible to
// ZendMM" would pass vacuously rather than meaningfully.
if (getenv('USE_ZEND_ALLOC') === '0') {
	echo 'skip ZendMM is disabled (USE_ZEND_ALLOC=0), so allocator routing is not measurable';
}
?>
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Sandbox;

// The =1 sibling proves flipping the toggle on routes the heap through
// ZendMM. This is the other direction, against the other regression: a
// change that made the flag inert by always routing through ZendMM would
// still pass that test, and only this one would notice. Off is the default,
// pinned first so the two assertions are about the same configuration.
var_dump(ini_get('luaext.use_zend_mm'));

$before = memory_get_usage();

$sandbox = new Sandbox();

// The same four megabytes of live Lua strings the sibling grows. On malloc,
// none of it appears in ZendMM's ledger.
[$held] = $sandbox->eval('
	local held = {}
	for index = 1, 64 do
		held[index] = string.rep(string.char(64 + index % 26), 65536)
	end
	return #held
', '=grow');

$grown = memory_get_usage() - $before;

var_dump($held, $grown < 1024 * 1024);

$sandbox->close();

?>
--EXPECT--
string(1) "0"
int(64)
bool(true)
