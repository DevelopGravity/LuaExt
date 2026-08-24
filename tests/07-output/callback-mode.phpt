--TEST--
Callback mode streams in order, chunked, and loses nothing at close
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
	outputCallback: static function (string $chunk, bool $isStderr) use (&$chunks): void {
		$chunks[] = [$chunk, $isStderr];
	},
	outputChunkBytes: 8,
));

// Each print ends in a newline, so each one is line-flushed; the long line goes
// out in whole chunks first and the remainder rides the newline out.
(void) $sandbox->eval('print("0123456789ABCDE") print("short")');

foreach ($chunks as [$chunk, $isStderr]) {
	printf("%s stderr=%s\n", json_encode($chunk), var_export($isStderr, true));
}

// Nothing is retrievable: it has already gone to the host.
var_dump($sandbox->getOutput());

// The budget still counts every byte that passed through.
var_dump($sandbox->getOutputLength());
var_dump($sandbox->stats()->outputBytes);

// Concatenating the chunks back together reproduces exactly what was printed,
// which is the whole promise of streaming: chunked, in order, nothing lost.
var_dump(implode('', array_column($chunks, 0)));

$sandbox->close();

?>
--EXPECT--
"01234567" stderr=false
"89ABCDE\n" stderr=false
"short\n" stderr=false
string(0) ""
int(22)
int(22)
string(22) "0123456789ABCDE
short
"
