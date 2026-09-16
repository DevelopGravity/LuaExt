--TEST--
An operator slot claimed by the array and by an attribute is refused either way round
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Exception\ConfigurationError;
use DevelopGravity\LuaExt\LuaMethod;
use DevelopGravity\LuaExt\LuaOperator;
use DevelopGravity\LuaExt\Operator;
use DevelopGravity\LuaExt\Sandbox;

// The two sources merge, and the duplicate guard is source-agnostic: a slot is
// claimed once, whether the claim came from the $operators array, from a
// #[LuaOperator] attribute, or one of each. The existing coverage exercises
// two array entries colliding; this is the cross-source collision.
final class Marked
{
	#[LuaMethod]
	public function value(): int { return 1; }

	#[LuaOperator(Operator::LessThan)]
	public function below(self $other): bool { return true; }

	#[LuaOperator(Operator::Add)]
	public function sum(self $other): int { return 2; }

	public function alsoBelow(self $other): bool { return false; }

	public function alsoSum(self $other): int { return 3; }
}

$attempts = [
	// The array names a different method for a slot the attribute already took.
	'array-vs-attribute' => ['alsoBelow' => Operator::LessThan],
	// The same for an arithmetic slot, to show it is not comparison-specific.
	'array-vs-attribute-arith' => ['alsoSum' => Operator::Add],
	// Two collisions at once: the first one found is the one reported.
	'both-slots' => ['alsoBelow' => Operator::LessThan, 'alsoSum' => Operator::Add],
];

foreach ($attempts as $label => $operators) {
	try {
		(new Sandbox())->registerClass(Marked::class, operators: $operators);
		echo $label, ": NOT REFUSED\n";
	} catch (ConfigurationError $error) {
		echo $label, ': ', $error->getMessage(), "\n";
	}
}

// Mapping the SAME method the attribute already mapped is the same collision:
// the slot is what is claimed, not the method.
try {
	(new Sandbox())->registerClass(Marked::class, operators: ['below' => Operator::LessThan]);
	echo "same-method: NOT REFUSED\n";
} catch (ConfigurationError $error) {
	echo 'same-method: ', $error->getMessage(), "\n";
}

// A slot the attributes left alone still maps from the array, alongside them:
// the array's Subtract and both attribute slots all dispatch on one class.
$sandbox = new Sandbox();
$sandbox->registerClass(Marked::class, operators: ['alsoSum' => Operator::Subtract], luaName: 'm');
$sandbox->setGlobal('a', new Marked());
$sandbox->setGlobal('b', new Marked());
var_dump($sandbox->eval('return a - b')[0]);
var_dump($sandbox->eval('return a + b')[0]);
var_dump($sandbox->eval('return a < b')[0]);
$sandbox->close();

?>
--EXPECT--
array-vs-attribute: Operator LessThan is mapped to two methods of Marked
array-vs-attribute-arith: Operator Add is mapped to two methods of Marked
both-slots: Operator LessThan is mapped to two methods of Marked
same-method: Operator LessThan is mapped to two methods of Marked
int(3)
int(2)
bool(true)
