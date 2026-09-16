--TEST--
An error value whose __tostring misbehaves is described, never converted
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Exception\RuntimeError;
use DevelopGravity\LuaExt\Sandbox;

// The traceback handler installed over every call is the last thing standing
// when a script errors, so it must not be able to fail: a handler that raises
// turns an ordinary error into LUA_ERRERR, which is the one status the host
// can do nothing useful with. It therefore DESCRIBES a non-string error value
// rather than converting it, and a hostile __tostring never runs.
//
// This is why ErrorHandlerError has no reachable path today, which its stub
// docblock records. Anything here starting to report ErrorHandlerError means
// the handler grew a conversion it should not have.
$cases = [
	'raises' => 'error(setmetatable({}, {__tostring = function() error("inner") end}))',
	'returns a table' => 'error(setmetatable({}, {__tostring = function() return {} end}))',
	'recurses forever' => 'local t t = setmetatable({}, {__tostring = function() return tostring(t) end}) error(t)',
	'errors with nil' => 'error(setmetatable({}, {__tostring = function() error() end}))',
	'no __tostring' => 'error(setmetatable({}, {}))',
];

foreach ($cases as $label => $code) {
	$sandbox = new Sandbox();

	try {
		(void) $sandbox->eval($code);
		echo $label, ": NOT RAISED\n";
	} catch (RuntimeError $error) {
		echo $label, ': ', $error->getMessage(), "\n";
	}

	$sandbox->close();
}

// A well-behaved __tostring is still honoured -- the refusal above is about
// handlers that misbehave, not about ignoring the metamethod.
$sandbox = new Sandbox();

try {
	(void) $sandbox->eval('error(setmetatable({}, {__tostring = function() return "polite" end}))');
	echo "polite: NOT RAISED\n";
} catch (RuntimeError $error) {
	echo 'polite: ', $error->getMessage(), "\n";
}

$sandbox->close();

?>
--EXPECTF--
raises: Lua raised a table error value
returns a table: Lua raised a table error value
recurses forever: Lua raised a table error value
errors with nil: Lua raised a table error value
no __tostring: table: %s
polite: %s
