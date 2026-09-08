--TEST--
Arity is exact at the boundary: no silent dropping, no silent absence
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Exception\RuntimeError;
use DevelopGravity\LuaExt\LuaMethod;
use DevelopGravity\LuaExt\Sandbox;

final class Dynamic
{
	public function __call(string $method, array $arguments): string
	{
		return $method . '/' . count($arguments);
	}
}

$sandbox = new Sandbox();
$sandbox->registerLibrary('t', [
	'pair' => static fn (int $a, int $b): int => $a + $b,
	'padded' => static fn (int $a, int $b = 10): int => $a + $b,
	'spread' => static fn (int ...$values): int => array_sum($values),
	'byref' => static function (int &$out): void { $out = 1; },
	'byrefSpread' => static function (int &...$outs): int { return count($outs); },
	'virtual' => [new Dynamic(), 'anything'],
]);

$probe = static function (string $label, string $expression) use ($sandbox): void {
	$result = $sandbox->eval(
		"local ok, value = pcall(function() return {$expression} end) " .
		'if ok then return "ok", tostring(value) end return "err", tostring(value)',
		'=probe',
	);

	printf("%-16s %s: %s\n", $label, $result[0], $result[1]);
};

// The floor: every required argument, or a refusal that names the shortfall.
$probe('missing', 't.pair(1)');
$probe('exact', 't.pair(1, 2)');

// Omitting an optional argument takes the PHP default; surplus is refused,
// never parked in func_get_args().
$probe('default', 't.padded(5)');
$probe('extra', 't.pair(1, 2, 3)');

// Variadics accept any surplus, and type-check every one of it.
$probe('variadic', 't.spread(1, 2, 3, 4)');
$probe('variadic-none', 't.spread()');
$probe('variadic-wrong', 't.spread(1, "2")');

// By-reference cannot cross from Lua, in fixed or variadic position.
$probe('byref', 't.byref(5)');
$probe('byref-variadic', 't.byrefSpread(1, 2)');

// A __call trampoline has no declared signature: only the floor applies.
$probe('trampoline', 't.virtual(1, "two", {})');

// The refusal is the script's to catch; uncaught, it reaches the host as a
// RuntimeError like every other request-validation refusal.
try {
	(void) $sandbox->eval('return t.pair(1)');
	echo "NOT REFUSED\n";
} catch (RuntimeError $error) {
	echo 'host sees: ', $error->getMessage(), "\n";
}

$sandbox->close();

?>
--EXPECT--
missing          err: pair expects at least 2 argument(s), 1 given
exact            ok: 3
default          ok: 15
extra            err: pair expects at most 2 argument(s), 3 given
variadic         ok: 10
variadic-none    ok: 0
variadic-wrong   err: spread: argument #2 ($values) must be of type int, string given
byref            err: byref: argument #1 ($out) is passed by reference, which cannot cross from Lua
byref-variadic   err: byrefSpread: argument #1 ($outs) is passed by reference, which cannot cross from Lua
trampoline       ok: anything/3
host sees: pair expects at least 2 argument(s), 1 given
