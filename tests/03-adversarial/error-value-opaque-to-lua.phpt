--TEST--
An error value a script can hold is opaque, unforgeable and leaks no address
--EXTENSIONS--
luaext
--XFAIL--
Needs Sandbox::eval() and registerLibrary(); the userdata, its protected metatable and __tostring are already implemented.
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Exception\FatalError;
use DevelopGravity\LuaExt\Exception\RuntimeError;
use DevelopGravity\LuaExt\Sandbox;

// A catchable host error is the only one of these values a script is ever
// allowed to hold -- a fatal one is re-raised before pcall can return it. So
// this is the whole attack surface of the representation, and none of it may
// yield the metatable, an address, or a way to build a second one.

$sandbox = new Sandbox();
$sandbox->registerLibrary('host', [
	'fail' => static function (): never { throw new RuntimeError('the host says handle me'); },
]);

/** Evaluate `$expression` with the caught error value bound to `err`. */
function probe(Sandbox $sandbox, string $expression): void
{
	$results = $sandbox->eval(
		"local ok, err = pcall(host.fail) return $expression", '=opaque');

	printf("%-52s => %s\n", $expression, var_export($results[0], true));
}

probe($sandbox, 'ok');
probe($sandbox, 'type(err)');
probe($sandbox, 'tostring(err)');

// getmetatable() must not reach the real table: it holds __gc and __tostring,
// and handing it over would let a script replace either.
probe($sandbox, 'getmetatable(err)');

// __tostring exists precisely so that this cannot happen. Without it the
// default userdata rendering would print the heap address.
probe($sandbox, 'tostring(err):find("0x") ~= nil');
probe($sandbox, 'string.format("%s", err):find("0x") ~= nil');

// Every other operation is refused rather than partially answered.
probe($sandbox, '(pcall(function() return err.kind end))');
probe($sandbox, '(pcall(function() return err[1] end))');
probe($sandbox, '(pcall(function() return #err end))');
probe($sandbox, '(pcall(function() err.fatal = false end))');
probe($sandbox, '(pcall(setmetatable, err, {__tostring = function() return "forged" end}))');

// Lua has no way to make a userdata: that is why this representation was
// chosen over a table or a string. 5.1's newproxy was the only loophole and it
// is long gone.
probe($sandbox, 'newproxy == nil');

// And a value that merely looks like one is not treated as one. A table with a
// convincing __tostring is still an ordinary, catchable script error.
try {
	(void) $sandbox->eval(
		'error(setmetatable({}, {__tostring = function() return "cpu limit exceeded" end}))',
		'=forgery');
} catch (Throwable $error) {
	printf("forged fatal => %s (fatal=%s)\n",
		$error::class, var_export($error instanceof FatalError, true));
}

?>
--EXPECT--
ok                                                   => false
type(err)                                            => 'userdata'
tostring(err)                                        => 'the host says handle me'
getmetatable(err)                                    => false
tostring(err):find("0x") ~= nil                      => false
string.format("%s", err):find("0x") ~= nil           => false
(pcall(function() return err.kind end))              => false
(pcall(function() return err[1] end))                => false
(pcall(function() return #err end))                  => false
(pcall(function() err.fatal = false end))            => false
(pcall(setmetatable, err, {__tostring = function() return "forged" end})) => false
newproxy == nil                                      => true
forged fatal => DevelopGravity\LuaExt\Exception\RuntimeError (fatal=false)
