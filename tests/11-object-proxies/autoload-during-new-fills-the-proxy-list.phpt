--TEST--
A nested push during .new cannot consume the slot the outer push is about to take
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\LuaMethod;
use DevelopGravity\LuaExt\Sandbox;

final class Filler
{
	#[LuaMethod]
	public function __construct() {}

	#[LuaMethod]
	public function id(): int
	{
		return 1;
	}
}

final class Latecomer
{
	// Resolving this default runs zend_update_class_constants() on the FIRST
	// instantiation, which reaches the autoloader -- host PHP, running inside
	// the window .new leaves open between its allocating and binding halves.
	public int $stamp = ProxyListMarker::VALUE;

	#[LuaMethod]
	public function __construct() {}

	#[LuaMethod]
	public function stamp(): int
	{
		return $this->stamp;
	}
}

$sandbox = new Sandbox();
$sandbox->registerClass(Filler::class, luaName: 'filler');
$sandbox->registerClass(Latecomer::class, luaName: 'late');

// eval() is how a phpt defines a class at autoload time; the code is a fixed
// literal, nothing user-supplied.
spl_autoload_register(static function (string $class) use ($sandbox): void {
	if ($class !== 'ProxyListMarker') {
		return;
	}

	// Exactly enough nested proxies to drive the live-proxy list onto its
	// capacity while the outer .new is mid-flight. A half that reserved a
	// slot before this ran would find it taken, and the outer bind would
	// write one past the end.
	//
	// The count is load-bearing: the list grows 0 -> 8 on the first bind, so
	// eight nested pushes land it exactly on the capacity. Seven or nine miss
	// the boundary and prove nothing. The overflow itself is an 8-byte write
	// that a plain allocator absorbs silently -- it faults reliably only
	// under a hardened one (verified with libgmalloc, and what the
	// USE_ZEND_ALLOC=0 sanitizer leg exists to catch). What this test pins
	// without help is the invariant either side of it: the count never
	// overshoots the capacity, so the list keeps growing afterwards.
	(void) $sandbox->eval('held = {} for i = 1, 8 do held[i] = filler.new() end');

	eval('final class ProxyListMarker { public const int VALUE = 41; }');
});

(void) $sandbox->eval('l = late.new()');
var_dump($sandbox->eval('return l:stamp()')[0]);
var_dump($sandbox->stats()->liveObjectProxies);

// The list must keep growing correctly afterwards. Once the count overshoots
// the capacity the "grow when count == cap" test can never match again, so
// every later push writes further past the end.
(void) $sandbox->eval('more = {} for i = 1, 100 do more[i] = filler.new() end');
var_dump($sandbox->stats()->liveObjectProxies);

(void) $sandbox->eval('l = nil held = nil more = nil collectgarbage("collect")');
var_dump($sandbox->stats()->liveObjectProxies);

// Everything still dispatches through a list that was rebuilt many times over.
var_dump($sandbox->eval('return filler.new():id()')[0]);

$sandbox->close();

?>
--EXPECT--
int(41)
int(9)
int(109)
int(0)
int(1)
