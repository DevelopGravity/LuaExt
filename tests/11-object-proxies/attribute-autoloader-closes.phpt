--TEST--
registerClass() refuses when materialising an attribute closed the sandbox
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Exception\ClosedSandboxError;
use DevelopGravity\LuaExt\LuaMethod;
use DevelopGravity\LuaExt\Operator;
use DevelopGravity\LuaExt\Sandbox;

final class NamedByConstant
{
	// The class itself is already loaded, so resolving it autoloads nothing.
	// Building the attribute OBJECT is what evaluates this argument, and a
	// class constant resolves through the autoloader -- arbitrary host PHP,
	// running after registration's only usability check.
	#[LuaMethod(MethodNameMarker::NAME)]
	public function ping(): int
	{
		return 1;
	}
}

final class OperatorNamedByConstant
{
	#[LuaMethod]
	public function value(): int
	{
		return 1;
	}

	#[\DevelopGravity\LuaExt\LuaOperator(OperatorMarker::SLOT)]
	public function plus(self $other): int
	{
		return 2;
	}
}

// eval() is how a phpt defines a class at autoload time; the code is a fixed
// literal, nothing user-supplied.
spl_autoload_register(static function (string $class) use (&$sandbox): void {
	if ($class === 'MethodNameMarker') {
		$sandbox->close();
		eval("final class MethodNameMarker { public const string NAME = 'ping'; }");
	}

	if ($class === 'OperatorMarker') {
		$sandbox->close();
		eval('final class OperatorMarker { public const '
			. Operator::class
			. ' SLOT = ' . Operator::class . '::Add; }');
	}
});

// A method attribute whose name argument autoloads.
$sandbox = new Sandbox();

try {
	$sandbox->registerClass(NamedByConstant::class);
	echo "NOT REFUSED\n";
} catch (ClosedSandboxError $error) {
	echo 'refused: ', $error->getMessage(), "\n";
}

// The same, one pass later: an operator attribute's slot argument autoloads,
// so the class survives method collection and dies during operator mapping.
$sandbox = new Sandbox();

try {
	$sandbox->registerClass(OperatorNamedByConstant::class);
	echo "NOT REFUSED\n";
} catch (ClosedSandboxError $error) {
	echo 'refused: ', $error->getMessage(), "\n";
}

?>
--EXPECT--
refused: The sandbox has been closed
refused: The sandbox has been closed
