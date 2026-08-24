--TEST--
The four output methods answer in every mode, and a script that printed nothing reads as nothing
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\OutputMode;
use DevelopGravity\LuaExt\Sandbox;
use DevelopGravity\LuaExt\SandboxConfig;

// getOutput() on a Discard-mode sandbox is the case worth pinning: the honest
// answer is an empty string, not an error. Throwing would make switching a
// sandbox to Discard break host code that merely reads its own output back.

$modes = [
	'Buffer' => new SandboxConfig(outputMode: OutputMode::Buffer),
	'Discard' => new SandboxConfig(outputMode: OutputMode::Discard),
	'Callback' => new SandboxConfig(
		outputMode: OutputMode::Callback,
		outputCallback: static function (string $chunk, bool $isStderr): void {
			echo "UNEXPECTED CHUNK: ", $chunk, "\n";
		},
	),
];

foreach ($modes as $name => $config) {
	$sandbox = new Sandbox($config);

	// Nothing has run, so there is nothing to report in any mode.
	printf(
		"%-8s output=%s length=%d truncated=%s take=%s\n",
		$name,
		var_export($sandbox->getOutput(), true),
		$sandbox->getOutputLength(),
		var_export($sandbox->isOutputTruncated(), true),
		var_export($sandbox->takeOutput(), true),
	);

	// stats() reads the same two fields the sink maintains, so it agrees for free.
	$stats = $sandbox->stats();
	printf(
		"%-8s stats.outputBytes=%d stats.outputTruncated=%s\n",
		$name,
		$stats->outputBytes,
		var_export($stats->outputTruncated, true),
	);

	// A script that emits nothing leaves all of it alone.
	var_dump($sandbox->eval('return 1 + 1'));
	printf("%-8s after-eval length=%d\n", $name, $sandbox->getOutputLength());

	$sandbox->close();
}

// A closed sandbox has no output to report, and says so as a closed sandbox
// rather than as an empty one.
$closed = new Sandbox();
$closed->close();

try {
	$closed->getOutput();
	echo "READ FROM A CLOSED SANDBOX\n";
} catch (Throwable $error) {
	printf("%s: %s\n", $error::class, $error->getMessage());
}

?>
--EXPECT--
Buffer   output='' length=0 truncated=false take=''
Buffer   stats.outputBytes=0 stats.outputTruncated=false
array(1) {
  [0]=>
  int(2)
}
Buffer   after-eval length=0
Discard  output='' length=0 truncated=false take=''
Discard  stats.outputBytes=0 stats.outputTruncated=false
array(1) {
  [0]=>
  int(2)
}
Discard  after-eval length=0
Callback output='' length=0 truncated=false take=''
Callback stats.outputBytes=0 stats.outputTruncated=false
array(1) {
  [0]=>
  int(2)
}
Callback after-eval length=0
DevelopGravity\LuaExt\Exception\ClosedSandboxError: The sandbox has been closed
