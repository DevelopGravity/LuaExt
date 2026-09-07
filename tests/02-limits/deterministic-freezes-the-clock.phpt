--TEST--
SandboxConfig::$deterministic pins the clock a script can see
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Capabilities;
use DevelopGravity\LuaExt\Sandbox;
use DevelopGravity\LuaExt\SandboxConfig;

// The flag's docblock promises a reproducible run. Fixing the seed was only
// half of that: os.time(), os.date() and os.clock() answered from the real
// clock, so the same chunk produced different bytes on every execution. A
// deterministic sandbox now answers from epoch zero -- and it does so with
// or without a fixed seed, so the flag is no longer a no-op on its own.
$chunk = '
	return table.concat({
		tostring(os.time()),
		os.date("!%Y-%m-%d %H:%M:%S"),
		string.format("%.1f", os.clock()),
	}, " | ")
';

$make = static function (bool $deterministic): Sandbox {
	return new Sandbox(new SandboxConfig(
		capabilities: (new Capabilities())->with(osTime: true),
		deterministic: $deterministic,
	));
};

$frozen = $make(true);

[$first] = $frozen->eval($chunk, '=frozen');
var_dump($first);

// Byte-stable across executions, which is what a golden-file run needs.
[$second] = $frozen->eval($chunk, '=again');
var_dump($first === $second);

$frozen->close();

// An explicit time argument stays the script's own: determinism freezes the
// DEFAULT, not arithmetic the script asked for.
$explicit = $make(true);
[$dated] = $explicit->eval('return os.date("!%Y", 946684800)', '=explicit');
var_dump($dated);
$explicit->close();

// And without the flag the real clock still shows.
$live = $make(false);
[$now] = $live->eval('return os.time()', '=live');
var_dump($now > 1000000000);
$live->close();

?>
--EXPECT--
string(29) "0 | 1970-01-01 00:00:00 | 0.0"
bool(true)
string(4) "2000"
bool(true)
