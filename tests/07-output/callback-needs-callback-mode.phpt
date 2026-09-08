--TEST--
An outputCallback the output mode will never call is refused, not silently dead
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Exception\ConfigurationError;
use DevelopGravity\LuaExt\OutputMode;
use DevelopGravity\LuaExt\Sandbox;
use DevelopGravity\LuaExt\SandboxConfig;

$callback = static function (string $chunk): void {};

// Default (Buffer) and explicit Discard both leave the callback dead.
$halfConfigured = [
	'default-mode' => new SandboxConfig(outputCallback: $callback),
	'discard-mode' => new SandboxConfig(
		outputMode: OutputMode::Discard,
		outputCallback: $callback,
	),
];

foreach ($halfConfigured as $label => $config) {
	try {
		new Sandbox($config);
		echo $label, ": NOT REFUSED\n";
	} catch (ConfigurationError $error) {
		echo $label, ': ', $error->getMessage(), "\n";
	}
}

// The properly paired form still works.
$paired = new Sandbox(new SandboxConfig(
	outputMode: OutputMode::Callback,
	outputCallback: static function (string $chunk): void { echo 'streamed: ', $chunk; },
));
(void) $paired->eval('print("hi")');
$paired->close();

?>
--EXPECT--
default-mode: SandboxConfig::$outputCallback does nothing without OutputMode::Callback. Pass outputMode: OutputMode::Callback to stream to it, or drop the callback.
discard-mode: SandboxConfig::$outputCallback does nothing without OutputMode::Callback. Pass outputMode: OutputMode::Callback to stream to it, or drop the callback.
streamed: hi
