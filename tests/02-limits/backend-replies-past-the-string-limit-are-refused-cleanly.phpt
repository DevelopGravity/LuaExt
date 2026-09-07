--TEST--
Backend and resolver replies past Limits::$maxStringLength are refused before Lua sees them
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

require __DIR__ . '/../06-vfs/memory-filesystem.inc';

use DevelopGravity\LuaExt\Capabilities;
use DevelopGravity\LuaExt\Exception\ModuleNotFoundError;
use DevelopGravity\LuaExt\Exception\VfsError;
use DevelopGravity\LuaExt\Limits;
use DevelopGravity\LuaExt\ModuleResolver;
use DevelopGravity\LuaExt\ModuleSource;
use DevelopGravity\LuaExt\Sandbox;
use DevelopGravity\LuaExt\SandboxConfig;

// The vendored interpreter refuses to CREATE a string past the limit, which
// means lua_pushlstring itself can raise -- and the three places that push a
// PHP backend's reply used to do so while still holding it, so the raise
// leaked the reply and (at the resolver) jumped out of a no-raise bracket.
// Each site now refuses first, releases first, and names its own boundary.

$capabilities = (new Capabilities())->with(require: true, vfs: true);
$limits = (new Limits())->with(maxStringLength: 64);
$oversized = str_repeat('-- padding line for an oversized module source' . "\n", 5);

// Door one: the VFS module search, FileSystem::read().
$sandbox = new Sandbox(new SandboxConfig(
	capabilities: $capabilities,
	limits: $limits,
	filesystem: new MemoryFileSystem(['/big.lua' => $oversized]),
));

try {
	(void) $sandbox->eval('return require("big")', '=r');
	print "vfs module  LOADED\n";
} catch (ModuleNotFoundError $error) {
	printf("vfs module  %s\n", $error->getMessage());
}
$sandbox->close();

// Door two: the module resolver, ModuleSource::$code.
final class OversizedResolver implements ModuleResolver
{
	public function __construct(private readonly string $code)
	{
	}

	public function resolve(string $module, string $requestedBy): ?ModuleSource
	{
		return new ModuleSource($this->code, '@big/' . $module);
	}
}

$sandbox = new Sandbox(new SandboxConfig(
	capabilities: (new Capabilities())->with(require: true),
	limits: $limits,
	moduleResolver: new OversizedResolver($oversized),
));

try {
	(void) $sandbox->eval('return require("big")', '=r');
	print "resolver    LOADED\n";
} catch (ModuleNotFoundError $error) {
	printf("resolver    %s\n", $error->getMessage());
}
$sandbox->close();

// Door three: a ranged read, RangedFileSystem::readRange().
$sandbox = new Sandbox(new SandboxConfig(
	capabilities: $capabilities,
	limits: $limits,
	filesystem: new RangedMemoryFileSystem(['/big.txt' => $oversized]),
));

try {
	(void) $sandbox->eval('local f = io.open("/big.txt", "r") return f:read("a")', '=r');
	print "ranged read LOADED\n";
} catch (VfsError $error) {
	printf("ranged read %s\n", $error->getMessage());
}
$sandbox->close();

?>
--EXPECT--
vfs module  That module's source exceeds Limits::$maxStringLength
resolver    That module's source or chunk name exceeds Limits::$maxStringLength
ranged read RangedFileSystem::readRange() returned more than Limits::$maxStringLength permits
