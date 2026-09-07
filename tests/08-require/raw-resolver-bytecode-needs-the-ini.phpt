--TEST--
luaext.allow_raw_bytecode=1 admits a resolver's raw bytecode, and still verifies seals
--EXTENSIONS--
luaext
--INI--
luaext.allow_raw_bytecode=1
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Capabilities;
use DevelopGravity\LuaExt\Exception\ModuleNotFoundError;
use DevelopGravity\LuaExt\ModuleResolver;
use DevelopGravity\LuaExt\ModuleSource;
use DevelopGravity\LuaExt\Sandbox;
use DevelopGravity\LuaExt\SandboxConfig;

// The ON half of the toggle resolver-bytecode-passes-the-same-vouching.phpt
// pins OFF: with the INI open, a raw blob a resolver hands back loads. What
// the INI must NOT change is seal verification -- a blob that claims to be
// sealed and does not verify stays refused, or the INI would be a bypass of
// the integrity check rather than a policy about unsealed bytes.

final class BlobResolver implements ModuleResolver
{
	/** @var array<string, string> */
	public array $blobs = [];

	public function resolve(string $module, string $requestedBy): ?ModuleSource
	{
		if (!isset($this->blobs[$module])) {
			return null;
		}

		return new ModuleSource($this->blobs[$module], '@blob/' . $module, isBytecode: true);
	}
}

$capabilities = (new Capabilities())->with(require: true, loadBytecode: true, dumpBytecode: true);
$resolver = new BlobResolver();
$sandbox = new Sandbox(new SandboxConfig(capabilities: $capabilities, moduleResolver: $resolver));

$sealed = $sandbox->compile('return { who = "raw" }', '@raw.lua')->dump(true);

$tampered = $sealed;
$tampered[strlen($tampered) - 1] = chr(ord($tampered[strlen($tampered) - 1]) ^ 0xFF);

// Strip the LXBC + version + algorithm + 16-byte xxh128 wrapper.
$resolver->blobs = ['raw' => substr($sealed, 22), 'tampered' => $tampered];

printf("raw       %s\n", $sandbox->eval('return require("raw").who', '=r')[0]);

try {
	(void) $sandbox->eval('return require("tampered")', '=r');
	print "tampered  LOADED\n";
} catch (ModuleNotFoundError $error) {
	print "tampered  refused\n";
}

$sandbox->close();

?>
--EXPECT--
raw       raw
tampered  refused
