--TEST--
Bytecode is refused unless it can be vouched for, in whichever way was asked for
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
use DevelopGravity\LuaExt\SealMode;

// WHAT THIS FILE IS FOR.
//
// Lua's binary loader validates a chunk's header, buffer bounds, constant tags
// and string indices -- and then stops. It does not check opcodes, register
// indices, constant indices or jump targets, and the checked fraction SHRINKS
// as blobs grow: flipping one byte at each position of a 150-byte chunk gave
// 57% refused / 23% right answer anyway / 8% WRONG answer / 13% dead process,
// while a 297 KB chunk gave only 17% refused and 82% ran.
//
// There is no verifier to add, so a blob is vouched for before the loader sees
// it, and anything that cannot be is refused.

$capabilities = (new Capabilities())->with(dumpBytecode: true, loadBytecode: true);
$keyOne = str_repeat("\x11", 32);
$keyTwo = str_repeat("\x22", 32);

$checksum = new Sandbox(new SandboxConfig(capabilities: $capabilities));
$authed = new Sandbox(new SandboxConfig(
	capabilities: $capabilities,
	sealMode: SealMode::Authenticated,
	bytecodeKey: $keyOne,
));

$source = 'local n = ... return n * 2';
$checksummed = $checksum->compile($source, '@double.lua')->dump(true);
$authenticated = $authed->compile($source, '@double.lua')->dump(true);

// The default needs no key and no INI: a dump loads back out of the box.
printf("checksum round-trips:  %d\n", $checksum->compileBinary($checksummed, '@d.lua')->call(21)[0]);
printf("authed round-trips:    %d\n", $authed->compileBinary($authenticated, '@d.lua')->call(21)[0]);

$refuses = static function (string $label, Sandbox $sandbox, string $bytecode): void {
	try {
		$sandbox->compileBinary($bytecode, '@probe.lua');
		printf("%-30s LOADED\n", $label);
	} catch (BytecodeIntegrityError $error) {
		printf("%-30s refused\n", $label);
	}
};

// A blob is verified against the mode the SANDBOX is configured for, never the
// one the blob announces, so the two modes do not interchange.
$refuses('checksum blob, authed sandbox', $authed, $checksummed);
$refuses('authed blob, checksum sandbox', $checksum, $authenticated);

// THE DOWNGRADE ATTACK, which is the reason the announced algorithm is not
// trusted. An attacker strips the HMAC and re-seals the same payload as a
// checksum -- which anyone can compute, since it is unkeyed. If the blob's own
// byte chose the check, the key would stop mattering entirely.
$payload = substr($authenticated, 6 + 32);
$forged = 'LXBC' . chr(1) . chr(1) . hash('xxh128', chr(1) . chr(1) . $payload, true) . $payload;

$refuses('downgraded blob, authed', $authed, $forged);

// And it IS a well-formed checksum blob -- the forgery is real, it is simply
// not accepted where authentication was asked for. An unkeyed checksum being
// forgeable is the documented property, not a defect: it answers "did this
// survive the trip", never "did this come from us".
printf("%-30s %s\n", 'forgery is a valid checksum', var_export(
	(static function (Sandbox $sandbox, string $blob): bool {
		try {
			$sandbox->compileBinary($blob, '@f.lua');

			return true;
		} catch (BytecodeIntegrityError $error) {
			return false;
		}
	})($checksum, $forged),
	true,
));

// Authentication's actual purchase: a blob sealed under one key does not load
// under another, so a bytecode store shared between processes fails CLOSED.
$stranger = new Sandbox(new SandboxConfig(
	capabilities: $capabilities,
	sealMode: SealMode::Authenticated,
	bytecodeKey: $keyTwo,
));

$refuses('their key, our sandbox', $authed, $stranger->compile('return 1', '@t.lua')->dump(true));
$stranger->close();

// The mode and the key have to agree; each half alone is a host saying one
// thing and getting another.
foreach ([
	'authed without a key' => static fn (): Sandbox => new Sandbox(new SandboxConfig(
		capabilities: $capabilities, sealMode: SealMode::Authenticated)),
	'checksum with a key' => static fn (): Sandbox => new Sandbox(new SandboxConfig(
		capabilities: $capabilities, bytecodeKey: $keyOne)),
	'key too short' => static fn (): Sandbox => new Sandbox(new SandboxConfig(
		capabilities: $capabilities, sealMode: SealMode::Authenticated, bytecodeKey: 'short')),
] as $label => $build) {
	try {
		$build();
		printf("%-30s ACCEPTED\n", $label);
	} catch (ConfigurationError $error) {
		printf("%-30s refused\n", $label);
	}
}

// THE SWEEP, in both modes. Every single-byte corruption and every truncation
// must be refused -- not merely most, which is all the bare loader manages.
foreach (['checksum' => [$checksum, $checksummed], 'authed' => [$authed, $authenticated]] as $mode => [$sandbox, $blob]) {
	$ran = 0;
	$refused = 0;

	for ($offset = 0; $offset < strlen($blob); $offset++) {
		$corrupt = $blob;
		$corrupt[$offset] = chr(ord($corrupt[$offset]) ^ 0xFF);

		try {
			$sandbox->compileBinary($corrupt, '@corrupt.lua')->call(1);
			$ran++;
		} catch (BytecodeIntegrityError $error) {
			$refused++;
		}
	}

	$short = 0;

	for ($length = 0; $length < strlen($blob); $length++) {
		try {
			$sandbox->compileBinary(substr($blob, 0, $length), '@short.lua');
		} catch (BytecodeIntegrityError $error) {
			$short++;
		}
	}

	// Byte counts are not asserted: they move with the Lua version and the
	// platform's integer widths. What matters is that none got through.
	printf("\n%s: every corruption refused: %s\n", $mode, var_export($refused === strlen($blob) && $ran === 0, true));
	printf("%s: every truncation refused: %s\n", $mode, var_export($short === strlen($blob), true));
}

$checksum->close();
$authed->close();

?>
--EXPECT--
checksum round-trips:  42
authed round-trips:    42
checksum blob, authed sandbox  refused
authed blob, checksum sandbox  refused
downgraded blob, authed        refused
forgery is a valid checksum    true
their key, our sandbox         refused
authed without a key           refused
checksum with a key            refused
key too short                  refused

checksum: every corruption refused: true
checksum: every truncation refused: true

authed: every corruption refused: true
authed: every truncation refused: true
