--TEST--
A chunk that does not parse reports its chunk name and line, and costs nothing
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Exception\SyntaxError;
use DevelopGravity\LuaExt\Limits;
use DevelopGravity\LuaExt\Sandbox;
use DevelopGravity\LuaExt\SandboxConfig;

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

// A parse failure never produced a Lua stack, and this file used to conclude
// from that the structured accessors must stay null -- leaving the single most
// useful line number in the extension reachable only by parsing getMessage().
//
// It is now synthesised instead. The chunk name is known, because the caller
// passed it in, and the line is taken from the message by stripping that known
// name rather than splitting on the first ':' -- which would read the wrong
// number for a chunk name that contains one.
try {
	$sandbox->compile('return (', '=probe');
} catch (SyntaxError $error) {
	var_dump($error->getLuaTrace(), $error->getChunkName(), $error->getLuaLine());
}

// The line is the parser's line, not the first one.
try {
	$sandbox->compile("local a = 1\nlocal b = 2\nreturn (", '=multiline');
} catch (SyntaxError $error) {
	var_dump($error->getLuaLine());
}

// A chunk name containing ':' is why the known name is stripped rather than
// searched for. Splitting on the first colon would report 'name' as the line.
try {
	$sandbox->compile('return (', '@weird:name:v2.lua');
} catch (SyntaxError $error) {
	var_dump($error->getChunkName(), $error->getLuaLine());
}

// An unprefixed chunk name is source text Lua quotes as [string "..."], so
// there is no name to match the message against and nothing is claimed. Saying
// nothing is the point: a guess here would be a wrong file name in a log.
try {
	$sandbox->compile('return (', 'plain');
} catch (SyntaxError $error) {
	var_dump($error->getChunkName(), $error->getLuaLine());
}

// A refusal that never reached the parser has no line, and must not be given
// one. maxSourceBytes throws a SyntaxError -- nothing is wrong with the chunk,
// it is simply too big -- and its message is not in the "name:line:" shape, so
// the parse finds nothing and nothing is claimed.
$bounded = new Sandbox(new SandboxConfig(limits: new Limits(maxSourceBytes: 8)));

try {
	$bounded->compile(str_repeat('x', 64), '@oversize.lua');
} catch (SyntaxError $error) {
	var_dump($error->getLuaLine());
}

$bounded->close();

// The synthesised frame has to satisfy the same validator that rejects a forged
// one, or a genuine syntax error would stop surviving a queue -- see
// tests/03-adversarial/unserialize-cannot-forge-a-traceback.phpt.
try {
	$sandbox->compile("\nreturn (", '=roundtrip');
} catch (SyntaxError $error) {
	$revived = unserialize(serialize($error));
	var_dump($revived->getChunkName(), $revived->getLuaLine());
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
array(1) {
  [0]=>
  array(6) {
    ["source"]=>
    string(5) "probe"
    ["what"]=>
    string(4) "main"
    ["currentLine"]=>
    int(1)
    ["name"]=>
    NULL
    ["nameWhat"]=>
    string(0) ""
    ["lineDefined"]=>
    int(0)
  }
}
string(5) "probe"
int(1)
int(3)
string(17) "weird:name:v2.lua"
int(1)
NULL
NULL
NULL
string(9) "roundtrip"
int(2)
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
