--TEST--
A compiled function dumps to bytecode and loads back as the same function
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Capabilities;
use DevelopGravity\LuaExt\Sandbox;
use DevelopGravity\LuaExt\SandboxConfig;

// dump() is the producing half of the dumpBytecode/loadBytecode pair;
// compileBinary() is the consuming half. They are separate capabilities on
// purpose: producing bytecode is safe, and loading it is arbitrary native
// execution, which is why loadBytecode stays off even under trusted().
//
// This exercises both halves together, because a dump nothing can load is not
// evidence of anything.

// Sealed, which is the supported way to round-trip: without a key the dump is
// a blob nothing can vouch for, and compileBinary() refuses it unless an
// operator has set luaext.allow_raw_bytecode. See the sealing test for that.
$capabilities = (new Capabilities())->with(dumpBytecode: true, loadBytecode: true);
$sandbox = new Sandbox(new SandboxConfig(
	capabilities: $capabilities,
	bytecodeKey: str_repeat('k', 32),
));

$function = $sandbox->compile('local a, b = ... return a * b + 1', '@math.lua');

$stripped = $function->dump();      // default is true
$full = $function->dump(false);

// Both are sealed: the seal's magic, then Lua's own ESC byte in the payload.
printf("stripped is sealed:       %s\n", var_export(substr($stripped, 0, 4) === 'LXBC', true));
printf("full is sealed:           %s\n", var_export(substr($full, 0, 4) === 'LXBC', true));
printf("payload is a Lua chunk:   %s\n", var_export(substr($stripped, 37, 4) === "\x1bLua", true));

// Stripping removes debug information, so it cannot be larger.
printf("stripping is smaller:     %s\n", var_export(strlen($stripped) < strlen($full), true));

// The point of the whole exercise: it loads, and it still computes.
foreach (['stripped' => $stripped, 'full' => $full] as $label => $bytecode) {
	$reloaded = $sandbox->compileBinary($bytecode, '@reloaded.lua');
	printf("%-9s round-trip: %d\n", $label, $reloaded->call(6, 7)[0]);
}

// Bytecode is binary and carries embedded NULs. A zend_string keeps its own
// length, so the value handed back is the whole chunk rather than everything up
// to the first NUL -- which is exactly the bug a strlen()-based path would have.
printf("contains NUL bytes:       %s\n", var_export(str_contains($stripped, "\0"), true));

// A dump is a value handed to PHP, so it is measured but not charged: nothing
// would ever give the budget back. Eight dumps must not walk the meter up.
$before = $sandbox->stats()->memoryBytes;

for ($index = 0; $index < 8; $index++) {
	$copy = $function->dump();
	unset($copy);
}

$after = $sandbox->stats()->memoryBytes;
printf("dumping is not billed:    %s\n", var_export($after <= $before * 2, true));

$sandbox->close();

?>
--EXPECT--
stripped is sealed:       true
full is sealed:           true
payload is a Lua chunk:   true
stripping is smaller:     true
stripped  round-trip: 43
full      round-trip: 43
contains NUL bytes:       true
dumping is not billed:    true
