--TEST--
The configuration objects a job payload carries survive serialization
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Capabilities;
use DevelopGravity\LuaExt\Limits;
use DevelopGravity\LuaExt\OutputMode;
use DevelopGravity\LuaExt\Sandbox;
use DevelopGravity\LuaExt\SandboxConfig;
use DevelopGravity\LuaExt\VfsQuota;

// A Sandbox cannot cross a process boundary -- it wraps a live lua_State, the
// same way a PDO handle wraps a live connection -- so the way to run Lua on a
// queue worker is to send the CONFIG and build the sandbox on the far side.
//
// That only works if the config objects serialize, which they do today by
// accident rather than by assertion. This pins it, so a future readonly field
// holding something exotic fails here rather than in someone's queue.

$roundTrips = static function (string $label, object $value): void {
	try {
		$copy = unserialize(serialize($value));
	} catch (Throwable $error) {
		printf("%-28s REFUSED: %s\n", $label, $error->getMessage());

		return;
	}

	printf("%-28s %s\n", $label, $copy::class === $value::class ? 'ok' : 'WRONG CLASS');
};

$roundTrips('Limits', new Limits(cpuSeconds: 1.5, memoryBytes: 4 * 1024 * 1024));
$roundTrips('Capabilities', new Capabilities());
$roundTrips('Capabilities::trusted', Capabilities::trusted());
$roundTrips('VfsQuota', new VfsQuota(maxFiles: 12));
// A plain Capabilities here on purpose: trusted() grants vfs, which needs a
// FileSystem, and that refusal is a different test's subject.
$roundTrips('SandboxConfig', new SandboxConfig(
	limits: new Limits(cpuSeconds: 2.0),
	capabilities: (new Capabilities())->with(compileAtRuntime: true),
));

// The values have to survive, not merely the class.
$limits = unserialize(serialize(new Limits(cpuSeconds: 1.5, maxModules: 9)));
printf("values kept: cpuSeconds=%s maxModules=%d\n",
	var_export($limits->cpuSeconds, true), $limits->maxModules);

$capabilities = unserialize(serialize(Capabilities::trusted()));
printf("values kept: require=%s vfsWrite=%s\n",
	var_export($capabilities->require, true), var_export($capabilities->vfsWrite, true));

// And the reconstructed config still builds a working sandbox, which is the
// whole point of sending it.
$config = unserialize(serialize(new SandboxConfig(limits: new Limits(cpuSeconds: 5.0))));
$sandbox = new Sandbox($config);
var_dump($sandbox->eval('return 6 * 7')[0]);
$sandbox->close();

// The one thing that does NOT travel, and the reason is worth stating: an output
// callback is a Closure, and PHP refuses to serialize those. A host queuing work
// re-attaches it worker-side. laravel/serializable-closure cannot substitute
// here, because the property is typed ?\Closure and that class is not a Closure.
try {
	serialize(new SandboxConfig(
		outputMode: OutputMode::Callback,
		outputCallback: static fn (string $chunk, bool $isStderr): null => null,
	));
	echo "closure: SERIALIZED\n";
} catch (Throwable $error) {
	printf("closure: %s\n", $error->getMessage());
}

// A Sandbox itself never travels. Stated here so the refusal is a documented
// contract rather than something discovered in production.
try {
	$live = new Sandbox();
	serialize($live);
	echo "sandbox: SERIALIZED\n";
} catch (Throwable $error) {
	printf("sandbox: %s\n", $error->getMessage());
}

$live->close();

?>
--EXPECT--
Limits                       ok
Capabilities                 ok
Capabilities::trusted        ok
VfsQuota                     ok
SandboxConfig                ok
values kept: cpuSeconds=1.5 maxModules=9
values kept: require=true vfsWrite=false
int(42)
closure: Serialization of 'Closure' is not allowed
sandbox: Serialization of 'DevelopGravity\LuaExt\Sandbox' is not allowed
