--TEST--
.new refuses when the autoloader it triggered unregistered the class mid-construction
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\LuaMethod;
use DevelopGravity\LuaExt\Sandbox;

final class Withdrawn
{
	// Resolving this default reaches the autoloader on the first
	// instantiation, after .new has already pushed its shell but before the
	// shell is bound -- and the shell pins nothing, so the record is free to
	// be reclaimed out from under the rest of the call.
	public int $stamp = WithdrawMarker::VALUE;

	#[LuaMethod]
	public function __construct() {}

	#[LuaMethod]
	public function stamp(): int
	{
		return $this->stamp;
	}
}

final class Bystander
{
	#[LuaMethod]
	public function __construct() {}

	#[LuaMethod]
	public function id(): int
	{
		return 2;
	}
}

$sandbox = new Sandbox();
$sandbox->registerClass(Withdrawn::class, luaName: 'gone');
$sandbox->registerClass(Bystander::class, luaName: 'bystander');

// eval() is how a phpt defines a class at autoload time; the code is a fixed
// literal, nothing user-supplied.
spl_autoload_register(static function (string $class) use ($sandbox): void {
	if ($class !== 'WithdrawMarker') {
		return;
	}

	// No proxy of this class is live, so retiring it reclaims the record
	// outright rather than merely retiring it.
	$sandbox->unregister('gone');

	eval('final class WithdrawMarker { public const int VALUE = 7; }');
});

// The constructor must re-read its registration across the object it created,
// not keep using the record it resolved before the autoloader ran.
var_dump($sandbox->eval('local ok, err = pcall(function() return gone.new() end) return ok, tostring(err)'));

// The withdrawal really did take effect, and the name is free to reclaim.
var_dump($sandbox->eval('return gone == nil')[0]);
var_dump($sandbox->stats()->liveObjectProxies);

// A sibling registration is untouched by the reclamation.
var_dump($sandbox->eval('return bystander.new():id()')[0]);

$sandbox->close();

?>
--EXPECT--
array(2) {
  [0]=>
  bool(false)
  [1]=>
  string(51) "gone.new cannot run: its registration was withdrawn"
}
bool(true)
int(0)
int(2)
