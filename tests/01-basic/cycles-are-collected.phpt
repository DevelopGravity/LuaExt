--TEST--
A reference cycle through a Sandbox or a LuaFunction is collected, not leaked
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\OutputMode;
use DevelopGravity\LuaExt\Sandbox;
use DevelopGravity\LuaExt\SandboxConfig;

// The host object graph routinely points back at the sandbox it serves: an
// output callback capturing the request context that owns the Sandbox is the
// everyday shape. The zvals carrying those references live in the C struct
// rather than the properties table, so it is the get_gc handler -- and only
// the get_gc handler -- that lets the cycle collector see them. Without it
// this cycle pins the object, and the whole malloc'd Lua heap behind it,
// until RSHUTDOWN.
$holder = new stdClass();
$config = new SandboxConfig(
	outputMode: OutputMode::Callback,
	outputCallback: static function (string $chunk, bool $isStderr) use ($holder): void {},
);
$sandbox = new Sandbox($config);
$holder->sandbox = $sandbox;

$weak = WeakReference::create($sandbox);

unset($sandbox, $config, $holder);
gc_collect_cycles();

var_dump($weak->get());

// The same through a LuaFunction handle: its reference to the owning sandbox
// is struct-held too, so this cycle runs handle -> sandbox -> callback ->
// holder -> handle and needs both handlers to be visible end to end.
$holder = new stdClass();
$config = new SandboxConfig(
	outputMode: OutputMode::Callback,
	outputCallback: static function (string $chunk, bool $isStderr) use ($holder): void {},
);
$sandbox = new Sandbox($config);
$holder->handle = $sandbox->compile('return "held by the cycle"');

$weakSandbox = WeakReference::create($sandbox);
$weakHandle = WeakReference::create($holder->handle);

unset($sandbox, $config, $holder);
gc_collect_cycles();

var_dump($weakSandbox->get(), $weakHandle->get());

?>
--EXPECT--
NULL
NULL
NULL
