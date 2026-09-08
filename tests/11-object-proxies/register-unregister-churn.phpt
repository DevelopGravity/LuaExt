--TEST--
Hundreds of register/unregister cycles reclaim every retired record and byte
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\LuaMethod;
use DevelopGravity\LuaExt\Sandbox;

final class Churn
{
	#[LuaMethod]
	public function __construct() {}

	#[LuaMethod]
	public function id(): int { return 1; }

	#[LuaMethod]
	public static function tag(): string { return 'c'; }
}

// luaext_proxy.h documents the invariant this loop leans on: a retired record
// lives exactly as long as something still dispatches through it, so swap
// loops cannot grow the retired chain without bound. Each cycle retires the
// class while a proxy is still live (parking the record), then drops the
// proxy (reclaiming it). Interned metatables and class tables live in the Lua
// registry, so unbounded growth there would show up in the Lua heap.
$sandbox = new Sandbox();

$cycle = static function () use ($sandbox): void {
	$sandbox->registerClass(Churn::class, luaName: 'C');
	$sandbox->setGlobal('p', new Churn());
	(void) $sandbox->eval('local n = p:id() + C.new():id() return n', '=use');
	$sandbox->unregister('C');
	(void) $sandbox->eval('p = nil collectgarbage("collect")', '=drop');
};

$settle = static function () use ($sandbox): int {
	(void) $sandbox->eval('collectgarbage("collect") collectgarbage("collect")', '=settle');

	return $sandbox->stats()->memoryBytes;
};

// Warm up past first-touch allocations, then measure the churn's residue.
for ($i = 0; $i < 10; $i++) {
	$cycle();
}
$baseline = $settle();

for ($i = 0; $i < 300; $i++) {
	$cycle();
}
$after = $settle();

printf("proxies drained: %d\n", $sandbox->stats()->liveObjectProxies);
printf("heap is steady:  %s\n", var_export($after - $baseline < 65536, true));

// And the three-hundred-and-eleventh registration behaves like the first.
$sandbox->registerClass(Churn::class, luaName: 'C');
printf("still correct:   %s / %d\n", Churn::tag(), $sandbox->eval('return C.new():id()')[0]);

$sandbox->close();

?>
--EXPECT--
proxies drained: 0
heap is steady:  true
still correct:   c / 1
