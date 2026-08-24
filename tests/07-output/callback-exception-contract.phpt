--TEST--
An output callback's RuntimeError is the script's to catch and anything else is not
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Exception\RuntimeError;
use DevelopGravity\LuaExt\OutputMode;
use DevelopGravity\LuaExt\Sandbox;
use DevelopGravity\LuaExt\SandboxConfig;

// The output callback is host code running from inside the interpreter, so it
// obeys the same contract every other host callback does: a RuntimeError is the
// host saying "the script is meant to handle this", and anything else is a
// failure the script had no part in and must not be able to swallow.

$refused = new RuntimeError('the log is full');

$catchable = new Sandbox(new SandboxConfig(
	outputMode: OutputMode::Callback,
	outputCallback: static function (string $chunk) use ($refused): never {
		throw $refused;
	},
	outputChunkBytes: 0,
));

var_dump($catchable->eval('local ok, err = pcall(print, "x") return ok, tostring(err)'));
$catchable->close();

// A broken host is not the script's business. pcall does not get a say, and
// what reaches the host is the object it threw rather than its message text.
$cause = new RuntimeException('the socket closed');
$broken = new LogicException('the log sink is misconfigured', 9, $cause);

$fatal = new Sandbox(new SandboxConfig(
	outputMode: OutputMode::Callback,
	outputCallback: static function (string $chunk) use ($broken): never {
		throw $broken;
	},
	outputChunkBytes: 0,
));

try {
	(void) $fatal->eval('pcall(print, "x") return "swallowed"', '=sink');
	echo "HOST FAILURE SWALLOWED\n";
} catch (LogicException $error) {
	printf(
		"class=%s same-object=%s code=%d previous-intact=%s\n",
		$error::class,
		var_export($error === $broken, true),
		$error->getCode(),
		var_export($error->getPrevious() === $cause, true),
	);
}

$fatal->close();

?>
--EXPECT--
array(2) {
  [0]=>
  bool(false)
  [1]=>
  string(15) "the log is full"
}
class=LogicException same-object=true code=9 previous-intact=true
