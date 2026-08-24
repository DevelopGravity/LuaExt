--TEST--
Lua coroutines are refused on the way to PHP
--EXTENSIONS--
luaext
--XFAIL--
Needs the coroutine library, which is only ever installed through the sandbox's own wrapper; the conversion side already refuses a thread.
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Exception\ConversionError;
use DevelopGravity\LuaExt\Sandbox;

$sandbox = new Sandbox();

// There is deliberately no PHP-side coroutine surface: every coroutine is
// force-closed when the outermost call returns, so a PHP handle onto one could
// only ever name a dead thread. Handing one out would promise a lifetime the
// sandbox does not offer.
//
// Userdata is refused for a different reason -- it is a raw pointer -- but an
// untrusted sandbox exposes no userdata to reach for; that case belongs to the
// VFS suite, where file handles are the first userdata a script can hold.
$rejected = [
	'returned directly' => 'return coroutine.create(function() end)',
	'inside a table' => 'return { worker = coroutine.create(function() end) }',
	'inside a sequence' => 'local co = coroutine.create(function() end); return { co }',
	'as a table key' => 'local co = coroutine.create(function() end); return { [co] = true }',
];

foreach ($rejected as $label => $chunk) {
	try {
		(void) $sandbox->eval($chunk);
		printf("%s: NOT REFUSED\n", $label);
	} catch (ConversionError $error) {
		printf("%s: %s\n", $label, $error::class);
	}
}

// A coroutine used as a coroutine, entirely inside the script, is unaffected:
// only crossing the boundary is refused.
var_dump($sandbox->eval(<<<'LUA'
	local co = coroutine.create(function(a) coroutine.yield(a + 1); return "done" end)
	local _, first = coroutine.resume(co, 1)
	local _, second = coroutine.resume(co)
	return first, second, coroutine.status(co)
	LUA));

?>
--EXPECT--
returned directly: DevelopGravity\LuaExt\Exception\ConversionError
inside a table: DevelopGravity\LuaExt\Exception\ConversionError
inside a sequence: DevelopGravity\LuaExt\Exception\ConversionError
as a table key: DevelopGravity\LuaExt\Exception\ConversionError
array(3) {
  [0]=>
  int(2)
  [1]=>
  string(4) "done"
  [2]=>
  string(4) "dead"
}
