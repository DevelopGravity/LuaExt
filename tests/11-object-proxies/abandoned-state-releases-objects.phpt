--TEST--
A bailout-abandoned sandbox sweeps its proxies' PHP references without incident
--EXTENSIONS--
luaext
--INI--
memory_limit=64M
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\LuaMethod;
use DevelopGravity\LuaExt\Sandbox;

// PHP's own memory_limit tripping inside a host callback is a zend_bailout: it
// longjmps past every C frame, leaving in_lua above zero, so close() refuses
// to lua_close() the abandoned state and its finalisers never run. The Lua
// heap is deliberately lost; the wrapped PHP objects are not — the RSHUTDOWN
// sweep releases each reference off the live-proxy list.
//
// What this test can assert is the sweep surviving live proxies at bailout:
// the request must end with the memory fatal and nothing else — no crash, no
// secondary error. The releases themselves are only visible to a debug-build
// allocator (they are what keeps its post-bailout leak report clean), and a
// destructor cannot witness them: after a fatal error the engine suppresses
// EVERY object destructor, ours included.

final class Held
{
	#[LuaMethod]
	public function id(): int { return 1; }
}

$sandbox = new Sandbox();
$sandbox->registerClass(Held::class);
$sandbox->setGlobal('h', new Held());
$sandbox->setGlobal('i', new Held());
$sandbox->setGlobal('j', new Held());

$sandbox->registerLibrary('boom', [
	'now' => static function (): int {
		str_repeat('x', 512 * 1024 * 1024);
		return 1;
	},
]);

(void) $sandbox->eval('return boom.now()');
echo "unreachable\n";

?>
--EXPECTF--
%AFatal error: Allowed memory size of %d bytes exhausted%A in %s on line %d%A
