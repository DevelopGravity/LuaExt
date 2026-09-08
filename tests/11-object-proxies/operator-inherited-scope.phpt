--TEST--
An inherited #[LuaOperator] resolves its arguments in the class that wrote it
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\LuaMethod;
use DevelopGravity\LuaExt\LuaOperator;
use DevelopGravity\LuaExt\Operator;
use DevelopGravity\LuaExt\Sandbox;

// The attribute's argument is a constant expression referencing a PRIVATE
// constant through self::. It must be evaluated in the scope of the class
// that declared the method -- registering a subclass used to hand the
// registering class as the scope, where the parent's private constant does
// not resolve.
abstract class VectorBase
{
	private const ADD_SLOT = Operator::Add;

	public function __construct(public readonly int $value) {}

	#[LuaMethod]
	public function get(): int { return $this->value; }

	#[LuaOperator(self::ADD_SLOT)]
	public function plus(self $other): static
	{
		return new static($this->value + $other->value);
	}
}

final class Vector extends VectorBase {}

$sandbox = new Sandbox();
$sandbox->registerClass(Vector::class);
$sandbox->setGlobal('a', new Vector(2));
$sandbox->setGlobal('b', new Vector(3));

var_dump($sandbox->eval('return (a + b):get()')[0]);

$sandbox->close();

?>
--EXPECT--
int(5)
