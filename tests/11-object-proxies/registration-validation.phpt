--TEST--
registerClass() refuses every malformed registration with a ConfigurationError
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Exception\ConfigurationError;
use DevelopGravity\LuaExt\LuaMethod;
use DevelopGravity\LuaExt\Sandbox;

interface Shape {}
enum Suit { case Hearts; }
abstract class AbstractBase
{
	#[LuaMethod]
	public function __construct() {}
}
final class Bare
{
	public function hidden(): int { return 1; }
}
final class Exposed
{
	#[LuaMethod]
	public function value(): int { return 42; }
}
final class TableClash
{
	#[LuaMethod]
	public static function new(): int { return 1; }

	#[LuaMethod]
	public function __construct() {}
}

$sandbox = new Sandbox();

$attempts = [
	'missing' => static fn () => $sandbox->registerClass('No\\Such\\ClassAtAll'),
	'interface' => static fn () => $sandbox->registerClass(Shape::class),
	'enum' => static fn () => $sandbox->registerClass(Suit::class),
	'abstract-ctor' => static fn () => $sandbox->registerClass(AbstractBase::class),
	'nothing' => static fn () => $sandbox->registerClass(Bare::class),
	'table-clash' => static fn () => $sandbox->registerClass(TableClash::class),
];

foreach ($attempts as $label => $attempt) {
	try {
		$attempt();
		echo $label, ": NOT REFUSED\n";
	} catch (ConfigurationError $error) {
		echo $label, ': ', $error->getMessage(), "\n";
	}
}

// A valid registration succeeds silently, and doing it twice is refused.
$sandbox->registerClass(Exposed::class);
echo "registered\n";

try {
	$sandbox->registerClass(Exposed::class);
} catch (ConfigurationError $error) {
	echo 'duplicate: ', $error->getMessage(), "\n";
}

// A second class may not claim the first one's Lua name.
final class ExposedToo
{
	#[LuaMethod]
	public static function make(): int { return 1; }
}

try {
	$sandbox->registerClass(ExposedToo::class, luaName: 'Exposed');
} catch (ConfigurationError $error) {
	echo 'name-clash: ', $error->getMessage(), "\n";
}

$sandbox->close();

?>
--EXPECT--
missing: Cannot register No\Such\ClassAtAll: the class does not exist
interface: Cannot register interface Shape: only classes have instances to proxy
enum: Cannot register enum Suit: enum cases are process-lifetime singletons, not instances a proxy can own
abstract-ctor: Cannot expose the constructor of abstract AbstractBase
nothing: Nothing of Bare is exposed: no method carries #[LuaMethod], no allowlist was given, and no operator is mapped
table-clash: Two exposures of TableClash both want the Lua name "new"
registered
duplicate: Exposed is already registered on this sandbox
name-clash: The Lua name "Exposed" is already taken by a previously registered class
