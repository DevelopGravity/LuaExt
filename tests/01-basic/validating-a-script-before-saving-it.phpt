--TEST--
validate() reports whether a chunk parses as data, without running or throwing
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Exception\ClosedSandboxError;
use DevelopGravity\LuaExt\Exception\SyntaxError;
use DevelopGravity\LuaExt\Limits;
use DevelopGravity\LuaExt\Sandbox;
use DevelopGravity\LuaExt\SandboxConfig;
use DevelopGravity\LuaExt\ValidationResult;

// The motivating case is a host storing user-authored Lua: reject a syntax
// error at save time and show the author the line, in the same request, without
// wrapping every save in a try/catch. compile() throws because a caller that
// asked for a function has nothing to do with a broken one; validate() is asked
// a question and answers it.

$sandbox = new Sandbox();

$report = static function (string $label, ValidationResult $result): void {
	printf(
		"%-16s valid=%-5s line=%-4s chunk=%s\n",
		$label,
		var_export($result->valid, true),
		var_export($result->line, true),
		var_export($result->chunkName, true),
	);
};

$report('valid', $sandbox->validate('return 1 + 1', '@ok.lua'));
$report('unclosed paren', $sandbox->validate("local x = 1\nreturn ((", '@bad.lua'));
$report('bad name', $sandbox->validate('local 1 = 2', '@names.lua'));

// The message is the parser's own, prefix included, so a host that would rather
// print one line than build its own has it.
printf("message: %s\n", $sandbox->validate('return ((', '@msg.lua')->message);

// An unprefixed chunk name would be source text to Lua, quoted as [string "..."],
// leaving nothing to strip off its message and therefore no line to report.
// validate() normalises it to "@plain" instead, because reporting a position is
// this method's entire purpose -- see the note on the helper in luaext_sandbox.c.
$report('unprefixed', $sandbox->validate('return ((', 'plain'));

// compile() deliberately does NOT normalise: it is a thin wrapper over Lua's
// loader and keeps Lua's convention. The divergence is intentional, and pinned
// here so it is not "fixed" later by someone who finds it surprising.
try {
	$sandbox->compile('return ((', 'plain');
} catch (SyntaxError $error) {
	printf("compile keeps Lua's convention: %s\n", var_export($error->getChunkName(), true));
}

// A refusal that never reached the parser has no line, and must not be given
// one: maxSourceBytes is a statement about size, not about a position.
$bounded = new Sandbox(new SandboxConfig(limits: new Limits(maxSourceBytes: 8)));
$report('over the limit', $bounded->validate(str_repeat('x', 64), '@big.lua'));
$bounded->close();

// Validating must not run the chunk, define anything, or leave the compiled
// function on the interpreter's stack -- a leaked slot per call would be a slow
// stack overflow in a service that validates on every save.
(void) $sandbox->validate('function sneaky() end return 1', '@effects.lua');
var_dump($sandbox->eval('return sneaky == nil')[0]);

$before = $sandbox->stats()->memoryBytes;

for ($index = 0; $index < 200; $index++) {
	(void) $sandbox->validate('local t = {1, 2, 3} return #t', '@loop.lua');
	(void) $sandbox->validate('return ((', '@loop.lua');
}

printf("200 rounds are flat: %s\n",
	var_export($sandbox->stats()->memoryBytes <= $before * 2, true));
printf("still usable: %d\n", $sandbox->eval('return 6 * 7')[0]);

// Serialises for a log or an API response.
echo json_encode($sandbox->validate('return ((', '@json.lua')), "\n";

// A ValidationResult travels, like every other config-shaped object here.
$revived = unserialize(serialize($sandbox->validate('return ((', '@queue.lua')));
$report('round-tripped', $revived);

// A host may build one when wrapping its own checks -- unlike SandboxStats,
// this object claims nothing that only the extension could know.
$report('host-built', new ValidationResult(false, 'my own rule', 12, 'mine.lua'));

$sandbox->close();

// A closed sandbox is a HOST problem, not a statement about the script, so it
// throws rather than coming back as valid=false.
try {
	(void) $sandbox->validate('return 1');
	echo "closed: RETURNED\n";
} catch (ClosedSandboxError $error) {
	printf("closed: %s\n", $error->getMessage());
}

?>
--EXPECT--
valid            valid=true  line=NULL chunk=NULL
unclosed paren   valid=false line=2    chunk='bad.lua'
bad name         valid=false line=1    chunk='names.lua'
message: msg.lua:1: unexpected symbol near <eof>
unprefixed       valid=false line=1    chunk='plain'
compile keeps Lua's convention: NULL
over the limit   valid=false line=NULL chunk=NULL
bool(true)
200 rounds are flat: true
still usable: 42
{"valid":false,"message":"json.lua:1: unexpected symbol near <eof>","line":1,"chunkName":"json.lua"}
round-tripped    valid=false line=1    chunk='queue.lua'
host-built       valid=false line=12   chunk='mine.lua'
closed: The sandbox has been closed
