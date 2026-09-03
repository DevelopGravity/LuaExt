--TEST--
Bytecode is refused unless it can be vouched for, and no corruption survives
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Capabilities;
use DevelopGravity\LuaExt\Exception\BytecodeIntegrityError;
use DevelopGravity\LuaExt\Exception\ConfigurationError;
use DevelopGravity\LuaExt\Sandbox;
use DevelopGravity\LuaExt\SandboxConfig;

// WHAT THIS FILE IS FOR.
//
// Lua's binary loader validates a chunk's header, buffer bounds, constant tags
// and string indices -- and then stops. It does not check opcodes, register
// indices, constant indices or jump targets, so corruption in the instruction
// stream reaches the VM intact. Measured by flipping one byte at each position
// of a 118-byte chunk and loading each: 57% refused, 33% RAN ANYWAY, and 10%
// killed the process outright.
//
// There is no verifier to add. So a blob is authenticated before the loader
// sees it, and anything that cannot be authenticated is refused.

$capabilities = (new Capabilities())->with(dumpBytecode: true, loadBytecode: true);
$keyOne = str_repeat("\x11", 32);
$keyTwo = str_repeat("\x22", 32);

$build = static fn (?string $key): Sandbox => new Sandbox(new SandboxConfig(
	capabilities: $capabilities,
	bytecodeKey: $key,
));

$sealed = $build($keyOne);
$unkeyed = $build(null);

$blob = $sealed->compile('local n = ... return n * 2', '@double.lua')->dump(true);
$raw = $unkeyed->compile('local n = ... return n * 2', '@double.lua')->dump(true);

printf("keyed dump is sealed:   %s\n", var_export(str_starts_with($blob, 'LXBC'), true));
printf("unkeyed dump is raw:    %s\n", var_export(str_starts_with($raw, "\x1bLua"), true));
printf("sealed round-trips:     %d\n", $sealed->compileBinary($blob, '@d.lua')->call(21)[0]);

// Every way a blob can fail to be vouched for. All one condition to the caller:
// this is not something the sandbox is willing to execute.
$refuses = static function (string $label, Sandbox $sandbox, string $bytecode): void {
	try {
		$sandbox->compileBinary($bytecode, '@probe.lua');
		printf("%-24s LOADED\n", $label);
	} catch (BytecodeIntegrityError $error) {
		printf("%-24s refused\n", $label);
	}
};

$refuses('wrong key', $build($keyTwo), $blob);
$refuses('no key for a seal', $unkeyed, $blob);
$refuses('raw, key configured', $sealed, $raw);
$refuses('raw, no key, INI off', $unkeyed, $raw);

// THE PROPERTY THAT MAKES "never share the bytecode store" ENFORCED rather than
// advised: a blob sealed by one process cannot be loaded by another that does
// not hold the same key. An attacker who can write the store has not gained
// anything, because what they write will not verify.
$other = $build($keyTwo);
$otherBlob = $other->compile('return "theirs"', '@theirs.lua')->dump(true);
$refuses('their blob, our key', $sealed, $otherBlob);
$other->close();

// A key too short to be a key authenticates nothing while looking as though it
// does, which is worse than having none.
try {
	$build('too-short');
	echo "short key: ACCEPTED\n";
} catch (ConfigurationError $error) {
	printf("short key: refused (%s)\n", str_contains($error->getMessage(), 'random_bytes') ? 'told how to fix it' : 'no guidance');
}

// THE SWEEP. Every single-byte corruption of a sealed blob must be refused --
// not merely most of them, which is what the bare loader manages. Nothing here
// may run, and nothing may crash.
$ran = 0;
$refused = 0;

for ($offset = 0; $offset < strlen($blob); $offset++) {
	$corrupt = $blob;
	$corrupt[$offset] = chr(ord($corrupt[$offset]) ^ 0xFF);

	try {
		$sealed->compileBinary($corrupt, '@corrupt.lua')->call(1);
		$ran++;
	} catch (BytecodeIntegrityError $error) {
		$refused++;
	}
}

// The byte count is not asserted: it moves with the Lua version and the
// platform's integer widths, and what matters is that NONE of them got through.
printf("\nsweep covered every byte:  %s\n", var_export($refused === strlen($blob), true));
printf("nothing ran:               %s\n", var_export($ran === 0, true));

// Truncation too, at every length. A short read from a cache is as likely as a
// flipped byte and just as unloadable.
$shortRefused = 0;

for ($length = 0; $length < strlen($blob); $length++) {
	try {
		$sealed->compileBinary(substr($blob, 0, $length), '@short.lua');
	} catch (BytecodeIntegrityError $error) {
		$shortRefused++;
	}
}

printf("every truncation refused: %s\n", var_export($shortRefused === strlen($blob), true));

$sealed->close();
$unkeyed->close();

?>
--EXPECT--
keyed dump is sealed:   true
unkeyed dump is raw:    true
sealed round-trips:     42
wrong key                refused
no key for a seal        refused
raw, key configured      refused
raw, no key, INI off     refused
their blob, our key      refused
short key: refused (told how to fix it)

sweep covered every byte:  true
nothing ran:               true
every truncation refused: true
