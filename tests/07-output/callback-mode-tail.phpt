--TEST--
A partial line reaches the callback when the sandbox closes rather than being dropped
--EXTENSIONS--
luaext
--XFAIL--
Needs io.write() from the library-policy wave. The close-time flush is wired and verified from the C side, but print() always ends in a newline, so a Lua-visible way to emit a partial line is required to exercise it -- and io is not opened yet.
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
