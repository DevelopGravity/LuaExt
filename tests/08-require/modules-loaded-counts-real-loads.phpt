--TEST--
stats()->modulesLoaded moves once per real load, and not for cache hits or failures
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

require __DIR__ . '/../06-vfs/memory-filesystem.inc';

use DevelopGravity\LuaExt\Capabilities;
use DevelopGravity\LuaExt\Exception\ModuleNotFoundError;
use DevelopGravity\LuaExt\Sandbox;
use DevelopGravity\LuaExt\SandboxConfig;

$sandbox = new Sandbox(new SandboxConfig(
	capabilities: (new Capabilities())->with(require: true, vfs: true),
	filesystem: new MemoryFileSystem([
		'/first.lua' => 'return { n = 1 }',
		'/second.lua' => 'return { n = 2 }',
	]),
));

var_dump($sandbox->stats()->modulesLoaded);

// One increment per module actually loaded.
(void) $sandbox->eval('require("first")', '=one');
var_dump($sandbox->stats()->modulesLoaded);

(void) $sandbox->eval('require("second")', '=two');
var_dump($sandbox->stats()->modulesLoaded);

// A cached re-require is a table lookup, not a load.
(void) $sandbox->eval('require("first") require("second")', '=cached');
var_dump($sandbox->stats()->modulesLoaded);

// A failed resolution loads nothing and counts nothing.
try {
	(void) $sandbox->eval('require("absent")', '=missing');
	echo "NOT REFUSED\n";
} catch (ModuleNotFoundError) {
	var_dump($sandbox->stats()->modulesLoaded);
}

$sandbox->close();

?>
--EXPECT--
int(0)
int(1)
int(2)
int(2)
int(2)
