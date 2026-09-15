--TEST--
A proxy yielded out of a coroutine keeps dispatching, and outlives the coroutine
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\LuaMethod;
use DevelopGravity\LuaExt\Sandbox;

final class Token
{
	public static int $destroyed = 0;

	#[LuaMethod]
	public function __construct(public readonly int $id = 0) {}

	#[LuaMethod]
	public function id(): int
	{
		return $this->id;
	}

	public function __destruct()
	{
		self::$destroyed++;
	}
}

$sandbox = new Sandbox();
$sandbox->registerClass(Token::class, luaName: 'token');

// A proxy minted on a coroutine's stack and yielded across the boundary is
// still a proxy on the resuming side: the live list belongs to the sandbox,
// not to whichever state happened to push it.
var_dump($sandbox->eval(<<<'LUA'
	local co = coroutine.create(function()
		coroutine.yield(token.new(7))
		return token.new(8)
	end)

	local _, first = coroutine.resume(co)
	local _, second = coroutine.resume(co)

	return first:id(), second:id(), coroutine.status(co)
LUA));

// Both are alive and reachable from the main state.
var_dump($sandbox->stats()->liveObjectProxies);

// A proxy held past the death of the coroutine that made it still dispatches:
// the coroutine is gone, collected, and the proxy's reference is its own.
var_dump($sandbox->eval(<<<'LUA'
	survivor = nil

	do
		local co = coroutine.create(function() coroutine.yield(token.new(9)) end)
		local _, value = coroutine.resume(co)
		survivor = value
	end

	collectgarbage("collect")
	collectgarbage("collect")

	return survivor:id()
LUA)[0]);

// The two from the first block became garbage when that chunk unwound; the
// survivor is still counted.
(void) $sandbox->eval('collectgarbage("collect")');
var_dump($sandbox->stats()->liveObjectProxies);

// Dropping the last reference hands the object back.
(void) $sandbox->eval('survivor = nil collectgarbage("collect")');
var_dump($sandbox->stats()->liveObjectProxies);
var_dump(Token::$destroyed);

$sandbox->close();

?>
--EXPECT--
array(3) {
  [0]=>
  int(7)
  [1]=>
  int(8)
  [2]=>
  string(4) "dead"
}
int(2)
int(9)
int(1)
int(0)
int(3)
