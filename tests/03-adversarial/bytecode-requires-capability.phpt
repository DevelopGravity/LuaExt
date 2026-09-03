--TEST--
Only compileBinary() with the loadBytecode capability will load a binary chunk
--EXTENSIONS--
luaext
--INI--
; This file's subject is the loadBytecode CAPABILITY, so the deployment gate
; is opened to get at it. The blob here comes from Lua's own string.dump and
; therefore carries no seal -- a script never sees the key. Whether the gate
; itself holds is tests/03-adversarial/sealed-bytecode-or-nothing.phpt.
luaext.allow_raw_bytecode=1
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Capabilities;
use DevelopGravity\LuaExt\Exception\CapabilityError;
use DevelopGravity\LuaExt\Exception\SyntaxError;
use DevelopGravity\LuaExt\Sandbox;
use DevelopGravity\LuaExt\SandboxConfig;

// Lua has never had a bytecode verifier and does not claim one. A crafted
// binary chunk is not a parse error, it is arbitrary native execution inside
// the host process, so the mode argument handed to the loader is the single
// thing standing between an untrusted string and that outcome.
//
// The blob is produced here rather than embedded, because bytecode is specific
// to the interpreter's version, integer width and endianness; a checked-in one
// would only prove that the test machine matches whoever wrote it.
$producer = new Sandbox(new SandboxConfig(
	capabilities: new Capabilities(dumpBytecode: true),
));

[$blob] = $producer->eval('return string.dump(function(n) return n * 2 end)');

var_dump(str_starts_with($blob, "\x1bLua"), strlen($blob) > 0);

// compile() is text-only whatever the sandbox was granted: `$code` is the one
// argument most likely to be attacker-controlled, and there is no capability
// that makes it safe to hand that string to the undumper.
$permissive = new Sandbox(new SandboxConfig(
	capabilities: new Capabilities(loadBytecode: true, dumpBytecode: true),
));

foreach (['untrusted' => $producer, 'loadBytecode granted' => $permissive] as $label => $sandbox) {
	try {
		$sandbox->compile($blob);
		printf("%s: compile() LOADED BYTECODE\n", $label);
	} catch (SyntaxError $error) {
		printf("%s: compile() refused, binary mentioned=%s\n",
			$label, var_export(str_contains($error->getMessage(), 'binary'), true));
	}
}

// Without the capability, compileBinary() refuses before the blob is looked at
// at all -- a CapabilityError, which is host misuse, not a malformed chunk.
try {
	$producer->compileBinary($blob);
	echo "NOT REFUSED\n";
} catch (CapabilityError $error) {
	printf("%s: %s\n", $error::class, $error->getMessage());
}

// A refusal is not a poisoned sandbox.
var_dump($producer->eval('return "still here"'));

// With the capability it loads and runs.
$doubler = $permissive->compileBinary($blob);
var_dump($doubler->call(21));

// And a blob that is not a chunk is a SyntaxError rather than a crash: the
// undumper's own header check is the last line of defence, which is exactly why
// it is not allowed to be the only one.
foreach (["\x1bLua\x00\x00\x00\x00truncated", "\x1bLua", "\x1bnot lua at all"] as $index => $corrupt) {
	try {
		$permissive->compileBinary($corrupt, '=corrupt');
		printf("corrupt %d: NOT REFUSED\n", $index);
	} catch (SyntaxError $error) {
		printf("corrupt %d: %s\n", $index, $error::class);
	}
}

// compileBinary() accepts source too: the capability widens what may be loaded
// rather than narrowing it, so a module loader holding a mix of both needs one
// entry point and not two.
var_dump($permissive->compileBinary('return "plain source"')->call());

$producer->close();
$permissive->close();

?>
--EXPECT--
bool(true)
bool(true)
untrusted: compile() refused, binary mentioned=true
loadBytecode granted: compile() refused, binary mentioned=true
DevelopGravity\LuaExt\Exception\CapabilityError: Loading precompiled bytecode requires the loadBytecode capability, which this sandbox was not granted
array(1) {
  [0]=>
  string(10) "still here"
}
array(1) {
  [0]=>
  int(42)
}
corrupt 0: DevelopGravity\LuaExt\Exception\SyntaxError
corrupt 1: DevelopGravity\LuaExt\Exception\SyntaxError
corrupt 2: DevelopGravity\LuaExt\Exception\SyntaxError
array(1) {
  [0]=>
  string(12) "plain source"
}
