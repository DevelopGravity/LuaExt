--TEST--
Limits::$maxSourceBytes is enforced before the parser sees the chunk
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Exception\SourceLimitError;
use DevelopGravity\LuaExt\Limits;
use DevelopGravity\LuaExt\Sandbox;
use DevelopGravity\LuaExt\SandboxConfig;

// Parsing is the one phase no interrupt can land in: the hook the CPU limit
// relies on runs between instructions of a chunk that already compiled, so a
// source pathological enough to make the parser itself expensive is bounded by
// its length or not at all. That is what this limit is for, and why it is
// checked before a byte reaches the loader rather than alongside it.
$sandbox = new Sandbox(new SandboxConfig(
	limits: new Limits(maxSourceBytes: 64),
));

// Exactly at the ceiling is inside it.
var_dump($sandbox->compile(str_repeat(' ', 64))->call());

// One byte over is refused, and the message names both figures so a host can
// tell "too big" from "malformed".
foreach ([65, 300] as $length) {
	try {
		$sandbox->compile(str_repeat('-', $length));
		printf("%d: NOT REFUSED\n", $length);
	} catch (SourceLimitError $error) {
		printf("%d: %s: %s\n", $length, $error::class, $error->getMessage());
	}
}

// eval() loads the same way, so it is bounded the same way.
try {
	(void) $sandbox->eval(str_repeat('--x', 100));
	echo "eval: NOT REFUSED\n";
} catch (SourceLimitError $error) {
	printf("eval: %s\n", $error->getMessage());
}

// The taxonomy, pinned deliberately rather than left incidental. This used to
// be a SyntaxError, and it is not one: the parser never saw the chunk, so there
// is nothing wrong with it and no line to point at. Calling it a syntax error
// sent whoever read the log hunting a mistake that was not there.
try {
	$sandbox->compile(str_repeat('-', 100));
} catch (SourceLimitError $error) {
	var_dump(
		$error instanceof DevelopGravity\LuaExt\Exception\FatalError,
		$error instanceof DevelopGravity\LuaExt\Exception\SyntaxError,
		$error->getLuaLine(),
	);
}

// Refusing is not truncating. A chunk cut to fit would compile something the
// caller did not write, which is a worse answer than an exception -- and the
// sandbox is untouched either way.
var_dump($sandbox->eval('return "still here"'));

// Zero means unlimited, as it does for every other limit.
$unbounded = new Sandbox(new SandboxConfig(
	limits: new Limits(maxSourceBytes: 0),
));

$long = str_repeat("-- filler\n", 20000) . 'return "long chunk"';
var_dump(strlen($long) > 64, $unbounded->eval($long));

$sandbox->close();
$unbounded->close();

?>
--EXPECT--
array(0) {
}
65: DevelopGravity\LuaExt\Exception\SourceLimitError: The chunk is 65 bytes, which exceeds the 64 byte source limit this sandbox was configured with
300: DevelopGravity\LuaExt\Exception\SourceLimitError: The chunk is 300 bytes, which exceeds the 64 byte source limit this sandbox was configured with
eval: The chunk is 300 bytes, which exceeds the 64 byte source limit this sandbox was configured with
bool(true)
bool(false)
NULL
array(1) {
  [0]=>
  string(10) "still here"
}
bool(true)
array(1) {
  [0]=>
  string(10) "long chunk"
}
