--TEST--
Operator mappings are validated at registration, not discovered at runtime
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Exception\ConfigurationError;
use DevelopGravity\LuaExt\LuaMethod;
use DevelopGravity\LuaExt\Operator;
use DevelopGravity\LuaExt\Sandbox;

final class Sample
{
	#[LuaMethod]
	public function id(): int { return 1; }

	public function looseReturn(self $other) { return true; }

	public function twoArgs(self $a, self $b): bool { return true; }

	public static function aStatic(self $other): bool { return true; }

	public function fine(self $other): bool { return true; }
}

$attempts = [
	'missing' => ['nothere' => Operator::LessThan],
	'loose-bool' => ['looseReturn' => Operator::LessThan],
	'arity' => ['twoArgs' => Operator::LessThan],
	'static' => ['aStatic' => Operator::LessThan],
	'dup-slot' => ['fine' => Operator::LessThan, 'looseReturn' => Operator::LessThan],
];

foreach ($attempts as $label => $operators) {
	try {
		(new Sandbox())->registerClass(Sample::class, operators: $operators);
		echo $label, ": NOT REFUSED\n";
	} catch (ConfigurationError $error) {
		echo $label, ': ', $error->getMessage(), "\n";
	}
}

?>
--EXPECT--
missing: Sample has no method nothere() to map to an operator
loose-bool: Sample::looseReturn() must declare a bool return to back a comparison operator
arity: Sample::twoArgs() must take exactly one required parameter to back a binary operator
static: Sample::aStatic() cannot back an operator: it is static
dup-slot: Operator LessThan is mapped to two methods of Sample
