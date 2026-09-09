--TEST--
Request shutdown survives a destructor closing a sibling sandbox mid-sweep
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\LuaMethod;
use DevelopGravity\LuaExt\Sandbox;

// The RSHUTDOWN sweep once cached a list cursor a destructor could free: a
// proxy's wrapped object, released while one sandbox closes, closing ANOTHER
// live sandbox mutated the very list being walked. This leaves three live
// sandboxes to the sweep, each holding a proxy whose destructor closes the
// next one in the ring — so whichever order the sweep walks, the first
// sandbox it closes detonates a close of a sibling it has not reached yet.
final class Grenade
{
	public ?Sandbox $target = null;

	#[LuaMethod]
	public function ping(): int { return 1; }

	public function __destruct()
	{
		try {
			$this->target?->close();
		} catch (\Throwable) {
			// The sweep got there first; that order is fine too.
		}

		$this->target = null;
	}
}

$a = new Sandbox();
$b = new Sandbox();
$c = new Sandbox();

foreach ([[$a, $b], [$b, $c], [$c, $a]] as [$holder, $victim]) {
	$holder->registerClass(Grenade::class);
	$grenade = new Grenade();
	$grenade->target = $victim;
	$holder->setGlobal('g', $grenade);
}

echo "reaching shutdown with three live sandboxes\n";

// Deliberately no close() and no unset: the request-shutdown sweep tears all
// three down, and the ring makes one of those closes happen mid-walk.

?>
--EXPECT--
reaching shutdown with three live sandboxes
