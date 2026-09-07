--TEST--
luaext.use_zend_mm=1 really routes the Lua heap through PHP's allocator
--EXTENSIONS--
luaext
--SKIPIF--
<?php
// run-tests.php -m (the valgrind leg) forces USE_ZEND_ALLOC=0, which turns
// ZendMM into a malloc pass-through that memory_get_usage() cannot see grow.
// Routing Lua's heap "through ZendMM" is then unobservable by design, not
// broken -- there is nothing this test could measure.
if (getenv('USE_ZEND_ALLOC') === '0') {
	echo 'skip ZendMM is disabled (USE_ZEND_ALLOC=0), so routed allocations are not measurable';
}
?>
--INI--
luaext.use_zend_mm=1
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Sandbox;

// This INI was registered, stored, displayed by phpinfo() and asserted by the
// build tests -- and read by nothing. Flipping it changed nothing at all. It
// now selects the allocator family at construction, so with it on the Lua
// heap is visible to memory_get_usage() and answers to PHP's memory_limit.
// Allocated bytes, not reserved chunks: ZendMM keeps freed chunks around
// for reuse, so the reserved figure would not shrink on close.
$before = memory_get_usage();

$sandbox = new Sandbox();

// Roughly four megabytes of live Lua strings, far above any allocator noise.
[$held] = $sandbox->eval('
	local held = {}
	for index = 1, 64 do
		held[index] = string.rep(string.char(64 + index % 26), 65536)
	end
	return #held
', '=grow');

$grown = memory_get_usage() - $before;

var_dump($held, $grown > 3 * 1024 * 1024);

// Closing hands it all back to the same allocator.
$sandbox->close();

var_dump(memory_get_usage() - $before < 1024 * 1024);

?>
--EXPECT--
int(64)
bool(true)
bool(true)
