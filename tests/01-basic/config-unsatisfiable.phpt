--TEST--
A configuration that cannot be satisfied is refused where it is built
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Capabilities;
use DevelopGravity\LuaExt\Exception\ConfigurationError;
use DevelopGravity\LuaExt\FileStat;
use DevelopGravity\LuaExt\FileSystem;
use DevelopGravity\LuaExt\Limits;
use DevelopGravity\LuaExt\SandboxConfig;

final class NullFileSystem implements FileSystem
{
	public function exists(string $path): bool
	{
		return false;
	}

	public function stat(string $path): ?FileStat
	{
		return null;
	}

	public function read(string $path): string
	{
		return '';
	}

	public function write(string $path, string $contents): void
	{
	}

	public function delete(string $path): void
	{
	}

	public function rename(string $from, string $to): void
	{
	}

	public function list(string $path): array
	{
		return [];
	}
}

function show(callable $attempt): void
{
	try {
		$attempt();
		echo "NOT REFUSED\n";
	} catch (ConfigurationError $error) {
		printf("%s\n\n", $error->getMessage());
	}
}

// A script that can install its own debug hook can displace the interpreter
// hook BOTH time limits are delivered through, so the pair is unsatisfiable,
// not merely unwise.
show(static fn () => new SandboxConfig(
	capabilities: new Capabilities(debugHooks: true),
));

// The wall-clock limit is covered too. Dropping only the CPU limit is not
// enough, because the watchdog thread just raises a flag and it is the same
// hook that turns that flag into a stopped script.
show(static fn () => new SandboxConfig(
	capabilities: new Capabilities(debugHooks: true),
	limits: (new Limits())->with(cpuSeconds: null),
));

// ... and it is the combination that is refused, not either half. Drop both
// time limits and the same capability is fine.
$hooked = new SandboxConfig(
	capabilities: new Capabilities(debugHooks: true),
	limits: (new Limits())->with(cpuSeconds: null, wallClockSeconds: null),
);
var_dump($hooked->capabilities?->debugHooks);

// Pinning the string hash seed forfeits hash-flooding protection.
show(static fn () => new SandboxConfig(seed: 42));

$deterministic = new SandboxConfig(seed: 42, deterministic: true);
var_dump($deterministic->seed, $deterministic->deterministic);

// The vfs capability with nothing behind it.
show(static fn () => new SandboxConfig(capabilities: new Capabilities(vfs: true)));

// Write without read. vfsWrite widens vfs rather than standing alone, so this
// describes a sandbox allowed to modify a store it may not open. Checked before
// the backing-store rule, and separately from it: supplying a filesystem does
// not make the combination coherent.
show(static fn () => new SandboxConfig(capabilities: new Capabilities(vfsWrite: true)));
show(static fn () => new SandboxConfig(
	capabilities: new Capabilities(vfsWrite: true),
	filesystem: new NullFileSystem(),
));

// Capabilities::trusted() enables vfs, so it needs a filesystem too. Supplying
// one is all it takes.
show(static fn () => new SandboxConfig(capabilities: Capabilities::trusted()));

$trusted = new SandboxConfig(
	capabilities: Capabilities::trusted(),
	filesystem: new NullFileSystem(),
);
var_dump($trusted->capabilities?->vfs, $trusted->filesystem instanceof FileSystem);

echo "--- reached through with() ---\n";

// with() can walk into an unsatisfiable combination the source did not have,
// so the derived object is checked too.
show(static fn () => $trusted->with(filesystem: null));
show(static fn () => (new SandboxConfig())->with(seed: 7));
show(static fn () => (new SandboxConfig())->with(capabilities: new Capabilities(debugHooks: true)));

// The source survives a refused derivation intact.
var_dump($trusted->filesystem instanceof FileSystem);

echo "--- negative limits ---\n";

// (size_t)-1 is the widest possible budget, so a negative limit is refused
// rather than converted.
show(static fn () => new SandboxConfig(limits: (new Limits())->with(memoryBytes: -1)));
show(static fn () => new SandboxConfig(limits: (new Limits())->with(cpuSeconds: -1.0)));
show(static fn () => new SandboxConfig(limits: (new Limits())->with(maxCallDepth: -5)));

// NAN is not a deadline either, and it compares false against everything, so
// the check is written to reject it rather than to accept it by omission.
show(static fn () => new SandboxConfig(limits: (new Limits())->with(cpuSeconds: NAN)));

// A limit larger than the clock can express saturates instead of wrapping: a
// huge ceiling must not turn into a tiny one.
$forever = new SandboxConfig(limits: (new Limits())->with(wallClockSeconds: INF));
var_dump($forever->limits?->wallClockSeconds);

?>
--EXPECTF--
The debugHooks capability cannot be combined with a CPU or wall-clock limit: a script that can call debug.sethook() replaces the interpreter hook BOTH limits are delivered through -- the watchdog thread only raises a flag, and that hook is what turns the flag into a stopped script -- so either limit would stop being enforced the moment the script chose to. Either drop debugHooks, or set both Limits::$cpuSeconds and Limits::$wallClockSeconds to null and accept that this sandbox cannot be bounded in time.

The debugHooks capability cannot be combined with a CPU or wall-clock limit: a script that can call debug.sethook() replaces the interpreter hook BOTH limits are delivered through -- the watchdog thread only raises a flag, and that hook is what turns the flag into a stopped script -- so either limit would stop being enforced the moment the script chose to. Either drop debugHooks, or set both Limits::$cpuSeconds and Limits::$wallClockSeconds to null and accept that this sandbox cannot be bounded in time.

bool(true)
A fixed SandboxConfig::$seed pins Lua's string hash seed, which forfeits the hash-flooding protection a random seed provides, so it has to be asked for explicitly: pass deterministic: true alongside it if this sandbox runs code you trust, or leave $seed null to draw one from the system CSPRNG.

int(42)
bool(true)
The vfs and vfsWrite capabilities need a backing store, but SandboxConfig::$filesystem is null. Pass an object implementing DevelopGravity\LuaExt\FileSystem, or turn them off with $capabilities->with(vfs: false, vfsWrite: false). Capabilities::trusted() enables vfs, so a trusted sandbox has to supply one too.

The vfsWrite capability widens vfs rather than replacing it, so it cannot be granted on its own: this configuration asks for a sandbox that may modify a filesystem it may not read. Pass $capabilities->with(vfs: true, vfsWrite: true) for read and write, or drop vfsWrite for read-only access.

The vfsWrite capability widens vfs rather than replacing it, so it cannot be granted on its own: this configuration asks for a sandbox that may modify a filesystem it may not read. Pass $capabilities->with(vfs: true, vfsWrite: true) for read and write, or drop vfsWrite for read-only access.

The vfs and vfsWrite capabilities need a backing store, but SandboxConfig::$filesystem is null. Pass an object implementing DevelopGravity\LuaExt\FileSystem, or turn them off with $capabilities->with(vfs: false, vfsWrite: false). Capabilities::trusted() enables vfs, so a trusted sandbox has to supply one too.

bool(true)
bool(true)
--- reached through with() ---
The vfs and vfsWrite capabilities need a backing store, but SandboxConfig::$filesystem is null. Pass an object implementing DevelopGravity\LuaExt\FileSystem, or turn them off with $capabilities->with(vfs: false, vfsWrite: false). Capabilities::trusted() enables vfs, so a trusted sandbox has to supply one too.

A fixed SandboxConfig::$seed pins Lua's string hash seed, which forfeits the hash-flooding protection a random seed provides, so it has to be asked for explicitly: pass deterministic: true alongside it if this sandbox runs code you trust, or leave $seed null to draw one from the system CSPRNG.

The debugHooks capability cannot be combined with a CPU or wall-clock limit: a script that can call debug.sethook() replaces the interpreter hook BOTH limits are delivered through -- the watchdog thread only raises a flag, and that hook is what turns the flag into a stopped script -- so either limit would stop being enforced the moment the script chose to. Either drop debugHooks, or set both Limits::$cpuSeconds and Limits::$wallClockSeconds to null and accept that this sandbox cannot be bounded in time.

bool(true)
--- negative limits ---
Limits::$memoryBytes is -1, and a limit cannot be negative. Pass 0 (or null, where the type allows it) to lift the limit, or a positive value to set one.

Limits::$cpuSeconds is -1, which is not a number of seconds a deadline can be set from. Pass null (or 0.0) to lift the limit, or a positive number of seconds to set one.

Limits::$maxCallDepth is -5, and a limit cannot be negative. Pass 0 (or null, where the type allows it) to lift the limit, or a positive value to set one.

Limits::$cpuSeconds is %s, which is not a number of seconds a deadline can be set from. Pass null (or 0.0) to lift the limit, or a positive number of seconds to set one.

float(INF)
