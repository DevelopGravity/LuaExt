--TEST--
A script-held proxy of an object referencing its own sandbox is a collectable cycle
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\LuaMethod;
use DevelopGravity\LuaExt\Sandbox;

final class HoldsSandbox
{
	public static int $alive = 0;

	public function __construct(public Sandbox $sandbox) { self::$alive++; }

	#[LuaMethod]
	public function id(): int { return 1; }

	public function __destruct() { self::$alive--; }
}

function build(): void
{
	$sandbox = new Sandbox();
	$sandbox->registerClass(HoldsSandbox::class);
	// Cycle: sandbox -> lua_State -> proxy -> object -> sandbox.
	$sandbox->setGlobal('keeper', new HoldsSandbox($sandbox));
	// Both locals go out of scope with the cycle intact.
}

build();
var_dump(HoldsSandbox::$alive);
gc_collect_cycles();
var_dump(HoldsSandbox::$alive);

?>
--EXPECT--
int(1)
int(0)
