--TEST--
with() refuses an unknown field by name, and refuses positional arguments outright
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Capabilities;
use DevelopGravity\LuaExt\Exception\ConfigurationError;
use DevelopGravity\LuaExt\Limits;
use DevelopGravity\LuaExt\SandboxConfig;
use DevelopGravity\LuaExt\VfsQuota;

function show(callable $attempt): void
{
	try {
		$attempt();
		echo "NOT REFUSED\n";
	} catch (ConfigurationError $error) {
		printf("%s\n", $error->getMessage());
	}
}

// A typo names a field that does not exist. The message names the offender and
// lists what could have been meant, because this is a development-time mistake
// and the useful answer is the candidate list.
show(static fn () => (new Capabilities())->with(vfsWrites: true));
show(static fn () => (new Limits())->with(cpuSecond: 2.0));
show(static fn () => (new VfsQuota())->with(maxHandles: 4));
show(static fn () => (new SandboxConfig())->with(filesystems: null));

echo "--- positional ---\n";

// with() exists to change one field by name. A positional form would have to
// mean "the Nth declared property", making stub declaration order part of the
// public API.
show(static fn () => (new Capabilities())->with(true));
show(static fn () => (new Limits())->with(1, 2, 3));

// Mixing the two is refused for the same reason, before any override is applied.
show(static fn () => (new VfsQuota())->with(4, billWallTime: true));

echo "--- nothing escaped ---\n";

// A refusal must not leave a half-built object behind, so the sources are
// unchanged and still usable.
$capabilities = new Capabilities();

try {
	$capabilities->with(vfs: true, nonsense: 1);
} catch (ConfigurationError) {
}

var_dump($capabilities->vfs, $capabilities->with(vfs: true)->vfs);

?>
--EXPECT--
DevelopGravity\LuaExt\Capabilities::with(): "vfsWrites" is not a property of DevelopGravity\LuaExt\Capabilities, so there is nothing for it to replace. Name one of: loadBytecode, compileAtRuntime, dumpBytecode, require, vfs, vfsWrite, coroutines, osTime, osEnv, osEnvAllowList, debugTraceback, debugIntrospect, debugMutate, debugHooks, utf8, gcControl, warn.
DevelopGravity\LuaExt\Limits::with(): "cpuSecond" is not a property of DevelopGravity\LuaExt\Limits, so there is nothing for it to replace. Name one of: memoryBytes, cpuSeconds, wallClockSeconds, outputBytes, outputOverflow, maxLiveCoroutines, maxCoroutineDepth, maxCallDepth, maxModules, maxRequireDepth, maxStringLength, maxSourceBytes, maxConversionDepth, maxCachedChunks.
DevelopGravity\LuaExt\VfsQuota::with(): "maxHandles" is not a property of DevelopGravity\LuaExt\VfsQuota, so there is nothing for it to replace. Name one of: maxOpenHandles, maxFileBytes, maxTotalBytes, maxFiles, maxOperations, maxPathLength, maxPathDepth, billWallTime.
DevelopGravity\LuaExt\SandboxConfig::with(): "filesystems" is not a property of DevelopGravity\LuaExt\SandboxConfig, so there is nothing for it to replace. Name one of: capabilities, limits, filesystem, vfsQuota, moduleResolver, modulePaths, outputMode, outputCallback, outputChunkBytes, seed, deterministic, cacheCompiledChunks, sealMode, bytecodeKey.
--- positional ---
DevelopGravity\LuaExt\Capabilities::with() takes named arguments only, and was given 1 positional one. Each override has to say which field it replaces, because a positional form would depend on the order the properties happen to be declared in. Write with(name: $value).
DevelopGravity\LuaExt\Limits::with() takes named arguments only, and was given 3 positional ones. Each override has to say which field it replaces, because a positional form would depend on the order the properties happen to be declared in. Write with(name: $value).
DevelopGravity\LuaExt\VfsQuota::with() takes named arguments only, and was given 1 positional one. Each override has to say which field it replaces, because a positional form would depend on the order the properties happen to be declared in. Write with(name: $value).
--- nothing escaped ---
bool(false)
bool(true)
