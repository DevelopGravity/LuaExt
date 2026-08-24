--TEST--
Dotted paths read, write and call through nested tables without metamethods
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Exception\RuntimeError;
use DevelopGravity\LuaExt\Sandbox;

$sandbox = new Sandbox();

// A path names a global, then a key inside it, and so on. Writing one creates
// the intermediates, because "set app.handlers.main" on a fresh sandbox is a
// reasonable thing for a host to ask for and building the tables by hand
// through eval() would be a worse way to do it.
$sandbox->setGlobal('app.handlers.main', 'ready');

var_dump($sandbox->eval('return type(app), type(app.handlers), app.handlers.main'));
var_dump($sandbox->getGlobal('app.handlers.main'));

// A single component is the plain global case.
$sandbox->setGlobal('bare', 1);
var_dump($sandbox->getGlobal('bare'));

// A missing leaf reads as null. So does anything under a missing intermediate:
// nothing below a table that does not exist can exist either, and reporting
// that as an error would make a host wrap every optional lookup in a try.
var_dump(
	$sandbox->getGlobal('absent'),
	$sandbox->getGlobal('app.handlers.absent'),
	$sandbox->getGlobal('absent.deeper.still'),
);

// An intermediate that does exist but cannot be indexed is a different answer:
// the path cannot mean anything, and null would report absence where the real
// problem is the question. The message names the prefix responsible.
$sandbox->setGlobal('number', 5);
$sandbox->setGlobal('nested.leaf', 'text');

foreach (['number.x', 'number.x.y', 'nested.leaf.deeper'] as $path) {
	foreach (['get', 'set'] as $direction) {
		try {
			$direction === 'get' ? $sandbox->getGlobal($path) : $sandbox->setGlobal($path, 1);
			printf("%s %s: NOT REFUSED\n", $direction, $path);
		} catch (RuntimeError $error) {
			printf("%s %s: %s\n", $direction, $path, $error->getMessage());
		}
	}
}

// Nothing was written on the way to any of those refusals.
var_dump($sandbox->eval('return number, nested.leaf'));

// A path that names nothing is host misuse rather than a script failure, and
// is reported against the argument like any other malformed argument.
foreach (['', 'a.', '.a', 'a..b', '.'] as $path) {
	try {
		$sandbox->getGlobal($path);
		printf("%s: NOT REFUSED\n", var_export($path, true));
	} catch (ValueError $error) {
		printf("%s: %s\n", var_export($path, true), $error->getMessage());
	}
}

// Storing null deletes the key: it is the only way to express "unset" from
// PHP, and a global holding a Lua nil is a global that is not there.
$sandbox->setGlobal('bare', null);
var_dump($sandbox->eval('return bare == nil'), $sandbox->getGlobal('bare'));

// Keys are taken as bytes, so a component is whatever sits between the dots --
// including one PHP would not accept as an identifier.
$sandbox->setGlobal('table with spaces', 'kept');
var_dump($sandbox->eval('return _G["table with spaces"]'));

// Traversal is raw in both directions. These run after a call has finished, so
// an __index or __newindex here would execute script code at a moment nothing
// is bounding it -- and would let a table lie to the host about its contents.
(void) $sandbox->eval(<<<'LUA'
	guarded = setmetatable({ real = 1 }, {
		__index = function() return "inherited" end,
		__newindex = function() error("__newindex ran during a host write") end,
	})
	LUA);

var_dump($sandbox->getGlobal('guarded.real'), $sandbox->getGlobal('guarded.missing'));

$sandbox->setGlobal('guarded.written', 'directly');
var_dump($sandbox->eval('return rawget(guarded, "written")'));

// call() resolves its target the same way, and says so when the path does not
// name something callable.
(void) $sandbox->eval('lib = { greet = function(name) return "hello " .. name end }');
var_dump($sandbox->call('lib.greet', 'world'));

foreach (['number', 'lib', 'lib.absent', 'absent.path'] as $path) {
	try {
		(void) $sandbox->call($path);
		printf("%s: NOT REFUSED\n", $path);
	} catch (RuntimeError $error) {
		printf("%s: %s\n", $path, $error->getMessage());
	}
}

$sandbox->close();

?>
--EXPECT--
array(3) {
  [0]=>
  string(5) "table"
  [1]=>
  string(5) "table"
  [2]=>
  string(5) "ready"
}
string(5) "ready"
int(1)
NULL
NULL
NULL
get number.x: Cannot resolve the Lua path "number.x": "number" is a number, which cannot be indexed
set number.x: Cannot resolve the Lua path "number.x": "number" is a number, which cannot be indexed
get number.x.y: Cannot resolve the Lua path "number.x.y": "number" is a number, which cannot be indexed
set number.x.y: Cannot resolve the Lua path "number.x.y": "number" is a number, which cannot be indexed
get nested.leaf.deeper: Cannot resolve the Lua path "nested.leaf.deeper": "nested.leaf" is a string, which cannot be indexed
set nested.leaf.deeper: Cannot resolve the Lua path "nested.leaf.deeper": "nested.leaf" is a string, which cannot be indexed
array(2) {
  [0]=>
  int(5)
  [1]=>
  string(4) "text"
}
'': DevelopGravity\LuaExt\Sandbox::getGlobal(): Argument #1 ($path) must name a Lua global, optionally with dotted components
'a.': DevelopGravity\LuaExt\Sandbox::getGlobal(): Argument #1 ($path) must not contain an empty path component
'.a': DevelopGravity\LuaExt\Sandbox::getGlobal(): Argument #1 ($path) must not contain an empty path component
'a..b': DevelopGravity\LuaExt\Sandbox::getGlobal(): Argument #1 ($path) must not contain an empty path component
'.': DevelopGravity\LuaExt\Sandbox::getGlobal(): Argument #1 ($path) must not contain an empty path component
array(1) {
  [0]=>
  bool(true)
}
NULL
array(1) {
  [0]=>
  string(4) "kept"
}
int(1)
NULL
array(1) {
  [0]=>
  string(8) "directly"
}
array(1) {
  [0]=>
  string(11) "hello world"
}
number: The Lua path "number" names a number, which is not callable
lib: The Lua path "lib" names a table, which is not callable
lib.absent: The Lua path "lib.absent" names a nil, which is not callable
absent.path: The Lua path "absent.path" names a nil, which is not callable
