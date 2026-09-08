--TEST--
A saved instance method refuses once its registration record is reclaimed
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\LuaMethod;
use DevelopGravity\LuaExt\Sandbox;

final class Alpha
{
	#[LuaMethod]
	public function ping(): string { return 'alpha'; }
}

final class Beta
{
	#[LuaMethod]
	public function pong(): string { return 'beta'; }
}

$sandbox = new Sandbox();
$sandbox->registerClass(Alpha::class);
$sandbox->registerLibrary('host', [
	'swap' => static function () use ($sandbox): bool {
		$sandbox->unregister('Alpha');
		$sandbox->registerClass(Beta::class);
		return true;
	},
	'retire' => static function () use ($sandbox): bool {
		$sandbox->unregister('Alpha');
		return true;
	},
	'beta' => static fn (): Beta => new Beta(),
]);

// The record dies with its last proxy. A method saved off a proxy, with the
// proxy collected and the class unregistered, must refuse by name — never
// judge through the freed record, whose storage the very next registration
// may reuse.
$sandbox->setGlobal('a', new Alpha());
var_dump($sandbox->eval(<<<'LUA'
	f = a.ping
	a = nil
	collectgarbage('collect')
	host.swap()
	b = host.beta()
	local ok, err = pcall(f, b)
	return ok, tostring(err)
LUA));

// Held proxies keep dispatching after unregister — the saved method included.
// The refusal begins only when the last proxy dies and the record goes with
// it, not a moment sooner.
$sandbox->registerClass(Alpha::class);
$sandbox->setGlobal('c', new Alpha());
var_dump($sandbox->eval(<<<'LUA'
	g = c.ping
	host.retire()
	local first = g(c)
	c = nil
	collectgarbage('collect')
	local ok, err = pcall(g, host.beta())
	return first, ok, tostring(err)
LUA));

$sandbox->close();

?>
--EXPECT--
array(2) {
  [0]=>
  bool(false)
  [1]=>
  string(49) "'ping' cannot run: its registration was withdrawn"
}
array(3) {
  [0]=>
  string(5) "alpha"
  [1]=>
  bool(false)
  [2]=>
  string(49) "'ping' cannot run: its registration was withdrawn"
}
