--TEST--
#[LuaClass] carries the maps; SandboxConfig::$classes grants at construction
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Exception\ConfigurationError;
use DevelopGravity\LuaExt\Exception\ConversionError;
use DevelopGravity\LuaExt\LuaClass;
use DevelopGravity\LuaExt\LuaOperator;
use DevelopGravity\LuaExt\Operator;
use DevelopGravity\LuaExt\Sandbox;
use DevelopGravity\LuaExt\SandboxConfig;

class Base
{
	public function __construct(protected int $value = 0) {}
	public static function make(int $value): static { return new static($value); }
	public function bump(): static { $this->value++; return $this; }
	public function value(): int { return $this->value; }
}

#[LuaClass(
	luaName: 'thing',
	methods: ['make', '__construct', 'bump', 'value'],
	operators: ['sameAs' => Operator::Equality],
)]
final class Wrapped extends Base
{
	// New methods may carry attributes -- only inherited ones cannot.
	#[LuaOperator(Operator::Add)]
	public function plus(self $other): static
	{
		return new static($this->value + $other->value);
	}

	public function sameAs(self $other): bool
	{
		return $this->value === $other->value;
	}
}

// Grant written ONCE on a reusable config; both sandboxes get the class.
$config = new SandboxConfig(classes: [Wrapped::class]);

foreach ([new Sandbox($config), new Sandbox($config)] as $sandbox) {
	var_dump($sandbox->eval('return thing.make(2):bump():value()')[0]);
	var_dump($sandbox->eval('return thing.new(1):value()')[0]);
	var_dump($sandbox->eval('return (thing.new(1) + thing.new(2)) == thing.new(3)')[0]);
	$sandbox->close();
}

// The attribute is a carrier, not a grant: a plain sandbox refuses instances.
$bare = new Sandbox();
try {
	$bare->setGlobal('w', new Wrapped(1));
} catch (ConversionError) {
	echo "carrier-not-grant\n";
}
$bare->close();

// Explicit parameters override the attribute's fields.
$override = new Sandbox();
$override->registerClass(Wrapped::class, luaName: 'other', methods: ['value', '__construct']);
var_dump($override->eval('return other.new(9):value(), thing == nil')[0]);
$override->close();

// with() carries the list like any other setting.
$viaWith = new Sandbox((new SandboxConfig())->with(classes: [Wrapped::class]));
var_dump($viaWith->eval('return thing.new(4):value()')[0]);
$viaWith->close();

// A duplicate in the config list fails Sandbox construction.
try {
	new Sandbox(new SandboxConfig(classes: [Wrapped::class, Wrapped::class]));
} catch (ConfigurationError $error) {
	echo 'dup: ', $error->getMessage(), "\n";
}

?>
--EXPECT--
int(3)
int(1)
bool(true)
int(3)
int(1)
bool(true)
carrier-not-grant
int(9)
int(4)
dup: Wrapped is already registered on this sandbox
