--TEST--
A chunk that does not parse reports its chunk name and line, and costs nothing
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Exception\SyntaxError;
use DevelopGravity\LuaExt\Sandbox;

$sandbox = new Sandbox();

/** Compile `$code` and report how the refusal identified itself. */
function refuse(Sandbox $sandbox, string $code, string $chunkName): void
{
	try {
		$sandbox->compile($code, $chunkName);
		echo "NOT REFUSED\n";
	} catch (SyntaxError $error) {
		printf("%s\n", $error->getMessage());
	}
}

// A syntax error has to say where it is or it is not actionable. Lua puts the
// chunk name and the line in front of every parser message, and the "=" prefix
// on a chunk name is what stops the interpreter quoting the script back at the
// caller in place of a name.
refuse($sandbox, 'return (', '=probe');
refuse($sandbox, 'local 1 = 2', '=probe');
refuse($sandbox, 'if true then', '=probe');
refuse($sandbox, 'return "unterminated', '=probe');

// The line is the line the parser failed on, not the first line of the chunk.
refuse($sandbox, "local a = 1\nlocal b = 2\nreturn (", '=multiline');

// A chunk name without the "=" prefix is treated as source text and quoted,
// which is Lua's own convention and the reason every default here carries one.
refuse($sandbox, 'return (', 'plain');

// eval() loads before it runs, so it refuses identically.
try {
	(void) $sandbox->eval('return (', '=evaluated');
} catch (SyntaxError $error) {
	printf("%s\n", $error->getMessage());
}

// A parse failure never produced a Lua stack -- there was no chunk to have one
// -- so the structured accessors are null rather than invented. The position
// lives in the message, which is where the parser put it.
try {
	$sandbox->compile('return (', '=probe');
} catch (SyntaxError $error) {
	var_dump($error->getLuaTrace(), $error->getChunkName(), $error->getLuaLine());
}

// SyntaxError is fatal, not a RuntimeError: a chunk that does not compile is
// not something a script could have been given the chance to handle.
try {
	$sandbox->compile('return (');
} catch (SyntaxError $error) {
	var_dump(
		$error instanceof DevelopGravity\LuaExt\Exception\FatalError,
		$error instanceof DevelopGravity\LuaExt\Exception\RuntimeError,
	);
}

// Nothing was left on the interpreter's stack by any of those failures, and no
// global was defined by the chunk that failed to compile.
try {
	$sandbox->compile('function leaked() end return (', '=partial');
} catch (SyntaxError $error) {
}

var_dump($sandbox->eval('return leaked == nil'));

// Valid source after the failures still compiles and runs.
var_dump($sandbox->compile('return 1 + 1')->call());

$sandbox->close();

?>
--EXPECT--
probe:1: unexpected symbol near <eof>
probe:1: <name> expected near '1'
probe:1: 'end' expected near <eof>
probe:1: unfinished string near <eof>
multiline:3: unexpected symbol near <eof>
[string "plain"]:1: unexpected symbol near <eof>
evaluated:1: unexpected symbol near <eof>
NULL
NULL
NULL
bool(true)
bool(false)
array(1) {
  [0]=>
  bool(true)
}
array(1) {
  [0]=>
  int(2)
}
