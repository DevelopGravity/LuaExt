--TEST--
The boundary is strict even when the host file never declared strict_types
--EXTENSIONS--
luaext
--FILE--
<?php

// Deliberately NO declare(strict_types=1): the gate must not inherit the
// ambient file's coercion mode. Every refusal below would be silently
// coerced if the boundary consulted this file's pragma.

use DevelopGravity\LuaExt\Sandbox;

$sandbox = new Sandbox();
$sandbox->registerLibrary('t', [
	'take' => static fn (int $value): int => $value + 1,
	'name' => static fn (string $name): string => $name,
	'flag' => static fn (bool $on): bool => $on,
]);

$probe = static function (string $label, string $expression) use ($sandbox): void {
	$result = $sandbox->eval(
		"local ok, value = pcall(function() return {$expression} end) " .
		'if ok then return "ok", tostring(value) end return "err", tostring(value)',
		'=probe',
	);

	printf("%-14s %s: %s\n", $label, $result[0], $result[1]);
};

$probe('string-to-int', 't.take("41")');
$probe('float-to-int', 't.take(2.5)');
$probe('int-to-string', 't.name(42)');
$probe('int-to-bool', 't.flag(1)');
$probe('int-ok', 't.take(41)');

$sandbox->close();

?>
--EXPECT--
string-to-int  err: take: argument #1 ($value) must be of type int, string given
float-to-int   err: take: argument #1 ($value) must be of type int, float given
int-to-string  err: name: argument #1 ($name) must be of type string, int given
int-to-bool    err: flag: argument #1 ($on) must be of type bool, int given
int-ok         ok: 42
