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
final class Sealed
{
	private function __construct() {}
}
final class Tricky
{
	public function __call(string $name, array $arguments): int { return 0; }
}
final class TrickyMarked
{
	#[LuaMethod]
	public function __get(string $name): int { return 0; }
}
final class NulName
{
	#[LuaMethod("evil\0hidden")]
	public function x(): int { return 0; }
}
final class NulCtor
{
	#[LuaMethod("bad\0name")]
	public function __construct() {}
}

$sandbox = new Sandbox();

$attempts = [
	'missing' => static function () use ($sandbox): void {
		$sandbox->registerClass('No\\Such\\ClassAtAll');
	},
	'interface' => static function () use ($sandbox): void {
		$sandbox->registerClass(Shape::class);
	},
	'enum' => static function () use ($sandbox): void {
		$sandbox->registerClass(Suit::class);
	},
	'abstract-ctor' => static function () use ($sandbox): void {
		$sandbox->registerClass(AbstractBase::class);
	},
	'nothing' => static function () use ($sandbox): void {
		$sandbox->registerClass(Bare::class);
	},
	'table-clash' => static function () use ($sandbox): void {
		$sandbox->registerClass(TableClash::class);
	},
	'private-ctor' => static function () use ($sandbox): void {
		$sandbox->registerClass(Sealed::class, methods: ['__construct']);
	},
	'magic-allowlisted' => static function () use ($sandbox): void {
		$sandbox->registerClass(Tricky::class, methods: ['__call']);
	},
	'magic-attributed' => static function () use ($sandbox): void {
		$sandbox->registerClass(TrickyMarked::class);
	},
	'nul-method-name' => static function () use ($sandbox): void {
		$sandbox->registerClass(NulName::class);
	},
	'nul-ctor-name' => static function () use ($sandbox): void {
		$sandbox->registerClass(NulCtor::class);
	},
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

// The two routed exceptions stay exposable when public: an allowlisted
// __construct becomes .new and __toString the metamethod.
final class Openly
{
	public function __construct(public readonly int $value = 7) {}

	public function __toString(): string { return "openly:{$this->value}"; }
}

$sandbox->registerClass(Openly::class, methods: ['__construct', '__toString']);
var_dump($sandbox->eval('return tostring(Openly.new())')[0]);

$sandbox->close();

?>
--EXPECT--
missing: Cannot register No\Such\ClassAtAll: the class does not exist
interface: Cannot register interface Shape: only classes have instances to proxy
enum: Cannot register enum Suit: enum cases are process-lifetime singletons, not instances a proxy can own
abstract-ctor: Cannot expose the constructor of abstract AbstractBase
nothing: Nothing of Bare is exposed: no method carries #[LuaMethod], no allowlist was given, and no operator is mapped
table-clash: Two exposures of TableClash both want the Lua name "new"
private-ctor: Sealed::__construct() cannot be exposed to Lua: it is not public
magic-allowlisted: Tricky::__call() cannot be exposed to Lua: it is a magic method, and magic methods are never exposed
magic-attributed: TrickyMarked::__get() carries #[LuaMethod] but cannot be exposed to Lua: it is a magic method, and magic methods are never exposed
nul-method-name: A Lua method name on NulName must be a non-empty string without NUL bytes
nul-ctor-name: A Lua method name on NulCtor must be a non-empty string without NUL bytes
registered
duplicate: Exposed is already registered on this sandbox
name-clash: The Lua name "Exposed" is already taken by an earlier registration on this sandbox
string(8) "openly:7"
