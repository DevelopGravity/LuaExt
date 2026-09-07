--TEST--
require() vouches for a resolver's bytecode the same way compileBinary() does
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Capabilities;
use DevelopGravity\LuaExt\Exception\ModuleNotFoundError;
use DevelopGravity\LuaExt\ModuleResolver;
use DevelopGravity\LuaExt\ModuleSource;
use DevelopGravity\LuaExt\Sandbox;
use DevelopGravity\LuaExt\SandboxConfig;

// A resolver handing back a binary module is the third door into Lua's
// unverified binary loader -- compileBinary() and the script's own load() are
// the other two -- and it answers the same one question: can this blob be
// vouched for? A sealed dump loads; a tampered seal is refused; an unsealed
// blob is refused while luaext.allow_raw_bytecode is off, which is the
// default this file runs under.

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

$sealed = $sandbox->compile('return { who = "sealed" }', '@sealed.lua')->dump(true);

$tampered = $sealed;
$tampered[strlen($tampered) - 1] = chr(ord($tampered[strlen($tampered) - 1]) ^ 0xFF);

// The seal wrapper is LXBC + version + algorithm + a 16-byte xxh128 under the
// default SealMode::Checksum; what follows is genuine raw bytecode.
$raw = substr($sealed, 22);

$resolver->blobs = ['sealed' => $sealed, 'tampered' => $tampered, 'raw' => $raw];

// The vouched-for blob loads and runs.
printf("sealed    %s\n", $sandbox->eval('return require("sealed").who', '=r')[0]);

// The other two are refused before the loader sees a byte, as the same
// catchable module error every other failed require step raises.
foreach (['tampered', 'raw'] as $module) {
	try {
		(void) $sandbox->eval(sprintf('return require("%s")', $module), '=r');
		printf("%-9s LOADED\n", $module);
	} catch (ModuleNotFoundError $error) {
		printf("%-9s refused: %s\n", $module, $error->getMessage());
	}
}

// Catchable from the script side too, like any other module failure.
var_dump($sandbox->eval('return (pcall(require, "raw"))', '=r')[0]);

$sandbox->close();

?>
--EXPECT--
sealed    sealed
tampered  refused: That module's bytecode does not verify: it was sealed by a sandbox configured differently -- another SealMode, or another SandboxConfig::$bytecodeKey -- or it has been altered since
raw       refused: That module is unsealed bytecode, which nothing can vouch for. Anything dump() produces is sealed and loads without any INI change; luaext.allow_raw_bytecode=1 is the only way to accept raw blobs
bool(false)
