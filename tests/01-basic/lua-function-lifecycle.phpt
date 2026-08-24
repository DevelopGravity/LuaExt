--TEST--
A LuaFunction keeps its sandbox alive and refuses use once that sandbox closes
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Exception\ClosedSandboxError;
use DevelopGravity\LuaExt\Exception\ConversionError;
use DevelopGravity\LuaExt\LuaFunction;
use DevelopGravity\LuaExt\Sandbox;

$sandbox = new Sandbox();
$compiled = $sandbox->compile('return "compiled"');

var_dump($compiled instanceof LuaFunction, $compiled->isValid(), $compiled->getSandbox() === $sandbox);
var_dump($compiled->call(), $compiled());

// Arguments cross on the way in and every result crosses on the way out, so a
// nil argument occupies its position rather than truncating the call -- which
// is what select("#") is asked here to prove.
[$echo] = $sandbox->eval('return function(...) return select("#", ...), ... end');

var_dump($echo->call(), $echo->call(1, 'two', null, [3]));

// A value with no Lua representation is refused before the function runs, and
// the sandbox is untouched by the refusal.
try {
	(void) $echo->call(new stdClass());
	echo "NOT REFUSED\n";
} catch (ConversionError $error) {
	printf("%s\n", $error::class);
}

var_dump($echo->call('after'));

// A handle is a second door onto one registry slot; a copy would be a second
// owner of it and would release it twice.
try {
	clone $compiled;
} catch (Error $error) {
	printf("%s: %s\n", $error::class, $error->getMessage());
}

// The handle holds a reference to its sandbox, because the slot it names lives
// in that sandbox's registry and the freelist it is returned to lives in the
// sandbox struct. Dropping the host's own variable must not take either away.
$scoped = new Sandbox();
$survivor = $scoped->compile('return "outlived the variable"');
unset($scoped);

var_dump($survivor->isValid(), $survivor->call());

// Closing explicitly is a different matter: the interpreter is gone, so the
// handle reports itself invalid and every use of it says why.
$sandbox->close();

var_dump($compiled->isValid(), $echo->isValid());

foreach (['call' => static fn(): array => $compiled->call(),
	'__invoke' => static fn(): array => $compiled()] as $label => $attempt) {
	try {
		(void) $attempt();
		printf("%s: NOT REFUSED\n", $label);
	} catch (ClosedSandboxError $error) {
		printf("%s: %s: %s\n", $label, $error::class, $error->getMessage());
	}
}

// getSandbox() still answers: the return type is not nullable, and a closed
// sandbox is perfectly capable of saying that it is closed.
var_dump($compiled->getSandbox() === $sandbox, $compiled->getSandbox()->isClosed());

// isValid() is the question a caller asks instead of risking an exception, so
// it never throws one -- including on a handle whose sandbox is long gone.
var_dump($compiled->isValid());

?>
--EXPECT--
bool(true)
bool(true)
bool(true)
array(1) {
  [0]=>
  string(8) "compiled"
}
array(1) {
  [0]=>
  string(8) "compiled"
}
array(1) {
  [0]=>
  int(0)
}
array(5) {
  [0]=>
  int(4)
  [1]=>
  int(1)
  [2]=>
  string(3) "two"
  [3]=>
  NULL
  [4]=>
  array(1) {
    [0]=>
    int(3)
  }
}
DevelopGravity\LuaExt\Exception\ConversionError
array(2) {
  [0]=>
  int(1)
  [1]=>
  string(5) "after"
}
Error: Trying to clone an uncloneable object of class DevelopGravity\LuaExt\LuaFunction
bool(true)
array(1) {
  [0]=>
  string(21) "outlived the variable"
}
bool(false)
bool(false)
call: DevelopGravity\LuaExt\Exception\ClosedSandboxError: The sandbox has been closed
__invoke: DevelopGravity\LuaExt\Exception\ClosedSandboxError: The sandbox has been closed
bool(true)
bool(true)
bool(false)
