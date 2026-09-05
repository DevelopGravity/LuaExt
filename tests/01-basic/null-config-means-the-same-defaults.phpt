--TEST--
Leaving limits or vfsQuota null resolves to exactly the documented defaults
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

require __DIR__ . '/../06-vfs/memory-filesystem.inc';

use DevelopGravity\LuaExt\Capabilities;
use DevelopGravity\LuaExt\Limits;
use DevelopGravity\LuaExt\Sandbox;
use DevelopGravity\LuaExt\SandboxConfig;
use DevelopGravity\LuaExt\VfsQuota;

// THE DEFAULTS ARE APPLIED BY TWO SEPARATE PIECES OF CODE, and nothing compared
// them until this test.
//
// Both read the same LUAEXT_DEFAULT_* macros, so the macros themselves are one
// source of truth and tests/01-basic/config-defaults.phpt already catches a
// change to one. What it cannot catch is the two READERS disagreeing:
//
//   luaext_config.c  Limits::__construct()             -- one local per field
//   luaext_config.c  luaext_config_default_limits()    -- one assignment per field
//
// The second is what `new Sandbox()` gets, because SandboxConfig::$limits may be
// null and null has to mean "the defaults" with no object to hold them. It is a
// hand-maintained parallel list of fourteen assignments, and a field added to
// one list and not the other -- or pointed at the wrong constant -- diverges
// silently, in the configuration nearly every host uses.
//
// Verified to catch exactly that: pointing default_limits()'s maxCallDepth at a
// literal while leaving the constructor alone fails this test and passes
// config-defaults.phpt. The two are complementary, not redundant.

$fromNull = new Sandbox(new SandboxConfig(limits: null));
$fromExplicit = new Sandbox(new SandboxConfig(limits: new Limits()));

$differences = 0;

foreach ((new ReflectionObject($fromNull->limits()))->getProperties() as $property) {
	$viaNull = $property->getValue($fromNull->limits());
	$viaExplicit = $property->getValue($fromExplicit->limits());

	if ($viaNull !== $viaExplicit) {
		printf(
			"DRIFT %s: null=%s explicit=%s\n",
			$property->getName(),
			var_export($viaNull, true),
			var_export($viaExplicit, true),
		);
		$differences++;
	}
}

printf("limits fields that differ: %d\n", $differences);

$fromNull->close();
$fromExplicit->close();

// VfsQuota has no accessor to compare field by field, so it is compared by what
// it DOES. Each refusal message names the bound it enforces, which is exactly
// the number that would drift.
$refusal = static function (?VfsQuota $quota, string $script): string {
	$sandbox = new Sandbox(new SandboxConfig(
		capabilities: (new Capabilities())->with(vfs: true, vfsWrite: true),
		filesystem: new MemoryFileSystem(),
		vfsQuota: $quota,
	));

	try {
		(void) $sandbox->eval($script, '=quota');
		$message = 'NOT REFUSED';
	} catch (Throwable $error) {
		$message = $error->getMessage();
	}

	$sandbox->close();

	return $message;
};

$probes = [
	'maxOpenHandles' => 'local kept = {} for i = 1, 64 do kept[i] = assert(io.open("/f" .. i, "w")) end',
	'maxFiles' => 'for i = 1, 512 do local f = assert(io.open("/n" .. i, "w")) f:close() end',
	'maxPathLength' => 'io.open("/" .. string.rep("a", 4096), "r")',
	'maxPathDepth' => 'io.open("/' . str_repeat('d/', 128) . 'f", "r")',
	'maxFileBytes' => 'local f = assert(io.open("/big", "w")) f:write(string.rep("x", 4194304))',
];

foreach ($probes as $name => $script) {
	$viaNull = $refusal(null, $script);
	$viaExplicit = $refusal(new VfsQuota(), $script);

	printf("%-16s %s\n", $name, $viaNull === $viaExplicit ? 'same' : "DIFFERS\n  null=$viaNull\n  explicit=$viaExplicit");
}

?>
--EXPECT--
limits fields that differ: 0
maxOpenHandles   same
maxFiles         same
maxPathLength    same
maxPathDepth     same
maxFileBytes     same
