--TEST--
A serialized payload cannot hand an exception a Lua traceback it never had
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Exception\ConfigurationError;
use DevelopGravity\LuaExt\Exception\RuntimeError;
use DevelopGravity\LuaExt\Sandbox;

// The Lua context is stored under engine-mangled keys naming a class that does
// not exist ("\0luaext\0luaTrace"), so no PHP SYNTAX can write one. unserialize()
// is not PHP syntax: it writes straight into the property table, and before the
// serialization handlers existed the forged value was read back and rendered as
// a working traceback.
//
// The fix is not to refuse serialization -- sandbox exceptions have to survive a
// queue -- but to own the unserialize path. What that buys, precisely:
//
//   - The MANGLED key is dead. __unserialize() reads the plain "luaTrace" data
//     key and nothing else, so the original direct-property attack writes a
//     property nothing ever reads.
//   - Through the PLAIN key, only a payload shaped exactly like a genuine
//     capture is stored; every malformed shape is dropped whole.
//   - A payload that IS shaped exactly right gets stored, deliberately: it is
//     indistinguishable from a genuine round-trip, and the traceback is
//     attribution data rather than a capability -- the same standing SECURITY.md
//     gives chunk names. A host that unserializes untrusted data has already
//     lost more than a traceback.
//
// The shape cases below use the PLAIN key, because that is the code path with
// branches to drive. An earlier version of this file sent every one of them
// through the mangled key, where they all "passed" without the validator
// running at all -- proven by the extra-keys case, which the validator of the
// day accepted and the test still called refused.

$mangled = static function (string $name): string {
	return "\0luaext\0" . $name;
};

$forge = static function (string $class, array $properties): string {
	$body = '';

	foreach ($properties as $key => $value) {
		$body .= sprintf('s:%d:"%s";%s', strlen($key), $key, serialize($value));
	}

	return sprintf('O:%d:"%s":%d:{%s}', strlen($class), $class, count($properties), $body);
};

$report = static function (string $label, string $payload): void {
	try {
		$object = unserialize($payload);
	} catch (Throwable $error) {
		printf("%-22s rejected at unserialize (%s)\n", $label, $error::class);

		return;
	}

	if (!$object instanceof RuntimeError) {
		printf("%-22s not an exception\n", $label);

		return;
	}

	printf(
		"%-22s trace=%s line=%s chunk=%s\n",
		$label,
		$object->getLuaTrace() === null ? 'null' : 'FORGED(' . count($object->getLuaTrace()) . ')',
		var_export($object->getLuaLine(), true),
		var_export($object->getChunkName(), true),
	);
};

// The original attack: a well-formed frame under the MANGLED key. Dead because
// nothing reads that key any more, not because the shape was judged.
$report('mangled-key forgery', $forge(RuntimeError::class, [
	$mangled('luaTrace') => [[
		'source' => '@totally-real.lua',
		'what' => 'Lua',
		'name' => 'adminOnly',
		'nameWhat' => 'global',
		'currentLine' => 1337,
		'lineDefined' => 1300,
	]],
]));

// The same frame under the PLAIN key is stored -- this is the documented
// accept, pinned so it cannot drift silently. See the header for why storing
// it is the design and not a hole.
$report('plain-key well-formed', $forge(RuntimeError::class, [
	'luaTrace' => [[
		'source' => '@totally-real.lua',
		'what' => 'Lua',
		'name' => 'adminOnly',
		'nameWhat' => 'global',
		'currentLine' => 1337,
		'lineDefined' => 1300,
	]],
]));

// Everything below is the surface the handler ADDS by accepting input at all.
// A type confusion here would be a worse bug than the forgery it closes, so each
// shape is driven explicitly -- through the plain key, where the validator
// actually runs.
$report('trace is a string', $forge(RuntimeError::class, ['luaTrace' => 'not a trace']));
$report('trace is an int', $forge(RuntimeError::class, ['luaTrace' => 42]));
$report('frame is a string', $forge(RuntimeError::class, ['luaTrace' => ['nope']]));
$report('frame is nested', $forge(RuntimeError::class, ['luaTrace' => [[['deep']]]]));
$report('fields wrong type', $forge(RuntimeError::class, [
	'luaTrace' => [['source' => 99, 'currentLine' => 'not-an-int']],
]));
$report('line is an array', $forge(RuntimeError::class, [
	'luaTrace' => [['source' => '@x.lua', 'currentLine' => ['boom']]],
]));

// Keys the capture path never emits are refused even when their values are
// well-typed. The validator used to allow "lastLineDefined" and "isTailCall"
// on the strength of lua_getinfo() knowing them -- but nothing here has ever
// written either, and a validator built on "refuse what capture does not emit"
// has to mean it.
$report('unemitted keys', $forge(RuntimeError::class, [
	'luaTrace' => [[
		'source' => '@x.lua',
		'what' => 'Lua',
		'name' => null,
		'nameWhat' => '',
		'currentLine' => 1,
		'lineDefined' => 1,
		'lastLineDefined' => 9,
		'isTailCall' => false,
	]],
]));

// Bounded: the capture path stops at LUAEXT_ERROR_TRACE_FRAMES (64), so a
// payload claiming ten thousand frames must not be honoured either.
$report('10k frames', $forge(RuntimeError::class, [
	'luaTrace' => array_fill(0, 10000, ['source' => '@x.lua', 'currentLine' => 1]),
]));

// A forged SANDBOX is the other half of the same idea -- getSandbox() must never
// hand back something a payload chose, under either key. There is no plain-key
// equivalent to pin: __unserialize() has no sandbox entry to read at all.
$report('forged sandbox key', $forge(RuntimeError::class, [
	$mangled('sandbox') => ['not', 'a', 'sandbox'],
]));

// And the genuine article still works, which is the whole point of not simply
// marking the class unserializable.
$sandbox = new Sandbox();

try {
	(void) $sandbox->eval('local function inner() error("boom") end inner()', '@real.lua');
} catch (RuntimeError $error) {
	$before = $error->getLuaTrace();
	$after = unserialize(serialize($error));

	printf("\nreal round-trip: frames %d -> %d\n", count($before), count($after->getLuaTrace()));
	printf("real round-trip: line %s -> %s\n",
		var_export($error->getLuaLine(), true), var_export($after->getLuaLine(), true));
	printf("real round-trip: chunk %s -> %s\n",
		var_export($error->getChunkName(), true), var_export($after->getChunkName(), true));

	// The sandbox is deliberately dropped: it holds a live lua_State, and a
	// revived one would be a lie. null is the honest answer.
	printf("real round-trip: sandbox %s -> %s\n",
		$error->getSandbox() === null ? 'null' : 'object',
		$after->getSandbox() === null ? 'null' : 'object');
	printf("real round-trip: message %s\n", var_export($after->getMessage() === $error->getMessage(), true));
}

$sandbox->close();

// A host-misuse exception carries no Lua context and must keep round-tripping
// exactly as it does today -- it is not part of this hierarchy's problem.
try {
	throw new ConfigurationError('plain host error', 7);
} catch (ConfigurationError $error) {
	$round = unserialize(serialize($error));
	printf("logic exception: %s / %d\n", $round->getMessage(), $round->getCode());
}

?>
--EXPECT--
mangled-key forgery    trace=null line=NULL chunk=NULL
plain-key well-formed  trace=FORGED(1) line=1337 chunk='@totally-real.lua'
trace is a string      trace=null line=NULL chunk=NULL
trace is an int        trace=null line=NULL chunk=NULL
frame is a string      trace=null line=NULL chunk=NULL
frame is nested        trace=null line=NULL chunk=NULL
fields wrong type      trace=null line=NULL chunk=NULL
line is an array       trace=null line=NULL chunk=NULL
unemitted keys         trace=null line=NULL chunk=NULL
10k frames             trace=null line=NULL chunk=NULL
forged sandbox key     trace=null line=NULL chunk=NULL

real round-trip: frames 3 -> 3
real round-trip: line 1 -> 1
real round-trip: chunk 'real.lua' -> 'real.lua'
real round-trip: sandbox object -> null
real round-trip: message true
logic exception: plain host error / 7
