--TEST--
memoryLimitBytes, luaCallsIn and gcCollections report behavior, not construction defaults
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Limits;
use DevelopGravity\LuaExt\Sandbox;
use DevelopGravity\LuaExt\SandboxConfig;

// The only prior test naming these fields pinned the JSON contract through
// reflection on a never-constructed object -- every value the PHP zero.
// These assertions bind each field to the behavior it claims to describe.

// memoryLimitBytes is the configured Limits::$memoryBytes...
$bounded = new Sandbox(new SandboxConfig(limits: new Limits(memoryBytes: 8388608)));
var_dump($bounded->stats()->memoryLimitBytes);
$bounded->close();

// ...and zero when the ceiling is lifted.
$unbounded = new Sandbox(new SandboxConfig(limits: new Limits(memoryBytes: null)));
var_dump($unbounded->stats()->memoryLimitBytes);
$unbounded->close();

$sandbox = new Sandbox();

// luaCallsIn counts entries from the host: one per eval(), call() and
// LuaFunction call -- including a compiled chunk's -- and nothing for the
// script's own internal calls, however many it makes.
var_dump($sandbox->stats()->luaCallsIn);

(void) $sandbox->eval('
	local function noise() return 1 end
	for index = 1, 100 do noise() end
	function named() return 2 end
', '=defines');

var_dump($sandbox->stats()->luaCallsIn);

(void) $sandbox->call('named');
$chunk = $sandbox->compile('return 3');
(void) $chunk->call();
(void) $chunk();

var_dump($sandbox->stats()->luaCallsIn);

// gcCollections moves with each collectgarbage("collect") the script issues,
// monotonically.
$before = $sandbox->stats()->gcCollections;

(void) $sandbox->eval('collectgarbage("collect") collectgarbage("collect")', '=collect');

$after = $sandbox->stats()->gcCollections;
var_dump($after - $before);

(void) $sandbox->eval('collectgarbage("collect")', '=again');
var_dump($sandbox->stats()->gcCollections - $after);

$sandbox->close();

?>
--EXPECT--
int(8388608)
int(0)
int(0)
int(1)
int(4)
int(2)
int(1)
