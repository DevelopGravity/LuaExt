--TEST--
Every proxy dispatch site follows the boundary exception rules, drain included
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Exception\RuntimeError;
use DevelopGravity\LuaExt\LuaMethod;
use DevelopGravity\LuaExt\LuaOperator;
use DevelopGravity\LuaExt\Operator;
use DevelopGravity\LuaExt\Sandbox;

final class Fragile
{
	#[LuaMethod]
	public function __construct(private bool $explode = false)
	{
		if ($this->explode) {
			throw new RuntimeError('constructor refused');
		}
	}

	#[LuaMethod]
	public static function boom(): never
	{
		throw new RuntimeError('static refused');
	}

	#[LuaMethod]
	public static function hardBoom(): never
	{
		throw new DomainException('static exploded');
	}

	#[LuaMethod]
	public function __toString(): string
	{
		throw new RuntimeError('unprintable');
	}

	#[LuaOperator(Operator::Add)]
	public function plus(self $other): self
	{
		throw new RuntimeError('cannot add');
	}
}

$sandbox = new Sandbox();
$sandbox->registerClass(Fragile::class);

// RuntimeError is catchable from every dispatch site: constructor, static,
// __tostring, and a mapped operator.
var_dump($sandbox->eval(<<<'LUA'
	local ctorOk, ctorErr = pcall(function() return Fragile.new(true) end)
	local staticOk = pcall(Fragile.boom)
	local tostringOk = pcall(function() return tostring(Fragile.new(false)) end)
	local a, b = Fragile.new(false), Fragile.new(false)
	local addOk = pcall(function() return a + b end)
	return ctorOk, tostring(ctorErr), staticOk, tostringOk, addOk
LUA));

// Anything else aborts the script and reaches the host as itself.
try {
	(void) $sandbox->eval('pcall(Fragile.hardBoom)');
	echo "NOT ABORTED\n";
} catch (DomainException $error) {
	echo 'aborted: ', $error->getMessage(), "\n";
}

// The refused constructions left no live proxies behind.
(void) $sandbox->eval('collectgarbage("collect")');
var_dump($sandbox->stats()->liveObjectProxies);
$sandbox->close();

// A __destruct that throws when the drain releases it reaches the host
// intact, and the sandbox survives to run again.
final class Grenade
{
	#[LuaMethod]
	public function id(): int { return 1; }

	public function __destruct()
	{
		throw new LogicException('destructor grenade');
	}
}

$second = new Sandbox();
$second->registerClass(Grenade::class);
$second->setGlobal('g', new Grenade());

try {
	(void) $second->eval('g = nil collectgarbage("collect") return 1');
	echo "NOT THROWN\n";
} catch (LogicException $error) {
	echo 'drain threw: ', $error->getMessage(), "\n";
}

var_dump($second->eval('return 2')[0]);
$second->close();

?>
--EXPECT--
array(5) {
  [0]=>
  bool(false)
  [1]=>
  string(19) "constructor refused"
  [2]=>
  bool(false)
  [3]=>
  bool(false)
  [4]=>
  bool(false)
}
aborted: static exploded
int(0)
drain threw: destructor grenade
int(2)
