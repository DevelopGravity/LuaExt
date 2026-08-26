--TEST--
A partial line reaches the callback when the sandbox closes rather than being dropped
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\OutputMode;
use DevelopGravity\LuaExt\Sandbox;
use DevelopGravity\LuaExt\SandboxConfig;

$chunks = [];

$sandbox = new Sandbox(new SandboxConfig(
	outputMode: OutputMode::Callback,
	outputCallback: static function (string $chunk) use (&$chunks): void {
		$chunks[] = $chunk;
	},
	outputChunkBytes: 4096,
));

// Short of the chunk threshold and short of a newline, so it sits in the buffer.
(void) $sandbox->eval('io.write("no newline here")');
var_dump($chunks);

// Closing is the last chance to hand it over, and it is taken.
$sandbox->close();
var_dump($chunks);

?>
--EXPECT--
array(0) {
}
array(1) {
  [0]=>
  string(15) "no newline here"
}
