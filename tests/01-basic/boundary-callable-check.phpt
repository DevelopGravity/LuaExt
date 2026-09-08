--TEST--
The callable check is the one gate branch that runs PHP, and its failures route honestly
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Exception\ConfigurationError;
use DevelopGravity\LuaExt\Sandbox;

$sandbox = new Sandbox();
$sandbox->registerLibrary('t', [
	'run' => static fn (callable $fn): string => 'ran',
]);

$probe = static function (string $label, string $expression) use ($sandbox): void {
	$result = $sandbox->eval(
		"local ok, value = pcall(function() return {$expression} end) " .
		'if ok then return "ok", tostring(value) end return "err", tostring(value)',
		'=probe',
	);

	printf("%-16s %s: %s\n", $label, $result[0], $result[1]);
};

// PHP's callable rules apply verbatim: a function-name string is callable,
// an unresolvable one is not. Hosts wanting only real functions should type
// the parameter LuaFunction (or Closure) instead -- documented.
$probe('fn', 't.run(function() end)');
$probe('name-string', 't.run("strtoupper")');
$probe('bad-string', 't.run("no_such_function_here")');

// Resolving "Class::method" fires the autoloader -- arbitrary host PHP, mid
// gate. An autoloader that defines nothing yields a plain refusal.
$defined = 0;
spl_autoload_register(static function (string $class) use (&$defined): void {
	if ($class === 'GateProbe\\Quiet') {
		$defined++;
	}
});
$probe('autoload-miss', 't.run("GateProbe\\\\Quiet::m")');
var_dump($defined);

// An autoloader that THROWS routes through the boundary's ordinary exception
// path: classified by class, here a LogicException -> fatal, host sees it.
spl_autoload_register(static function (string $class): void {
	if ($class === 'GateProbe\\Loud') {
		throw new LogicException('autoloader objects');
	}
});

try {
	(void) $sandbox->eval('pcall(function() return t.run("GateProbe\\\\Loud::m") end)');
	echo "NOT SURFACED\n";
} catch (LogicException $error) {
	echo 'host sees: ', $error->getMessage(), "\n";
}

// An autoloader that tries to close the sandbox mid-check meets the same
// in-flight guard every host callback does.
spl_autoload_register(static function (string $class) use ($sandbox): void {
	if ($class === 'GateProbe\\Saboteur') {
		$sandbox->close();
	}
});

try {
	(void) $sandbox->eval('pcall(function() return t.run("GateProbe\\\\Saboteur::m") end)');
	echo "closed under us\n";
} catch (ConfigurationError $error) {
	echo 'close refused: ', $error->getMessage(), "\n";
}

// And the sandbox survived all of it.
var_dump($sandbox->eval('return t.run(function() end)')[0]);

$sandbox->close();

?>
--EXPECT--
fn               ok: ran
name-string      ok: ran
bad-string       err: run: argument #1 ($fn) must be of type callable, string given
autoload-miss    err: run: argument #1 ($fn) must be of type callable, string given
int(1)
host sees: autoloader objects
close refused: Cannot close a sandbox while it is running: close() was called from inside a call into Lua, whose state is still executing
string(3) "ran"
