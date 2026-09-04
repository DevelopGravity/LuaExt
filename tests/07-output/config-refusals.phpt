--TEST--
A sink that could not do what it was asked for is refused at construction
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Exception\ConfigurationError;
use DevelopGravity\LuaExt\OutputMode;
use DevelopGravity\LuaExt\Sandbox;
use DevelopGravity\LuaExt\SandboxConfig;

// SandboxConfig accepts these fields without looking at them together, so the
// sink checks them at the first moment anything depends on them: construction.
// A host learns that its output would go nowhere on the line that built the
// sandbox rather than by wondering where its output went.

try {
	new Sandbox(new SandboxConfig(outputMode: OutputMode::Callback));
	echo "ACCEPTED A CALLBACK SINK WITH NOWHERE TO CALL\n";
} catch (ConfigurationError $error) {
	echo $error->getMessage(), "\n";
}

// Negative is not a size. Left unchecked it becomes an enormous size_t and the
// sink silently buffers the whole run instead of streaming it.
try {
	new Sandbox(new SandboxConfig(outputChunkBytes: -1));
	echo "ACCEPTED A NEGATIVE CHUNK SIZE\n";
} catch (ConfigurationError $error) {
	echo $error->getMessage(), "\n";
}

// Discard is the deliberate way to say "nowhere", and it needs no callback.
$discarding = new Sandbox(new SandboxConfig(outputMode: OutputMode::Discard));
var_dump($discarding->getOutput());
$discarding->close();

// Zero is a legitimate chunk size: hand every write straight over.
$streaming = new Sandbox(new SandboxConfig(
	outputMode: OutputMode::Callback,
	outputCallback: static function (string $chunk, bool $isStderr): void {},
	outputChunkBytes: 0,
));
var_dump($streaming->stats()->outputBytes);
$streaming->close();

?>
--EXPECT--
OutputMode::Callback needs a SandboxConfig::$outputCallback to stream to. Without one a script's output would go nowhere, which OutputMode::Discard says deliberately.
SandboxConfig::$outputChunkBytes is -1, which is not a number of bytes to buffer. Pass 0 to hand every write straight to the callback.
string(0) ""
int(0)
