--TEST--
Output is bytes: NUL and invalid UTF-8 pass through unchanged
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\OutputMode;
use DevelopGravity\LuaExt\Sandbox;
use DevelopGravity\LuaExt\SandboxConfig;

// A Lua string is a byte string. Anything here that stopped at a NUL, or that
// tried to be clever about encoding, would corrupt a script writing a binary
// payload -- and would do it silently.

$payload = 'a\0b\xff\xfe\xc3\x28';

$sandbox = new Sandbox();
(void) $sandbox->eval(sprintf('print("%s")', $payload));

$buffered = $sandbox->getOutput();
var_dump(bin2hex($buffered));
var_dump($sandbox->stats()->outputBytes);
$sandbox->close();

// The same bytes reach a streaming host, still unexamined.
$chunks = [];
$streaming = new Sandbox(new SandboxConfig(
	outputMode: OutputMode::Callback,
	outputCallback: static function (string $chunk) use (&$chunks): void {
		$chunks[] = $chunk;
	},
));

(void) $streaming->eval(sprintf('print("%s")', $payload));
$streaming->close();

var_dump(bin2hex(implode('', $chunks)));
var_dump(implode('', $chunks) === $buffered);

?>
--EXPECT--
string(16) "610062fffec3280a"
int(8)
string(16) "610062fffec3280a"
bool(true)
