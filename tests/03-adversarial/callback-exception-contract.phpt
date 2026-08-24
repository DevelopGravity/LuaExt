--TEST--
A callback's RuntimeError is the script's to catch and anything else is not
--EXTENSIONS--
luaext
--XFAIL--
Needs Sandbox::eval() and registerLibrary() wired to the callback bridge; the bridge, the retention and the rethrow are implemented.
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Exception\LuaThrowable;
use DevelopGravity\LuaExt\Exception\RuntimeError;
use DevelopGravity\LuaExt\Sandbox;

// The whole callback boundary rests on one distinction. A RuntimeError is the
// host saying "the script is meant to handle this", so pcall may catch it.
// Anything else -- a TypeError in the callback, a driver failing, a LogicException
// like the one here -- is somebody else's failure, and letting a script swallow
// it would mean untrusted code deciding whether the host's problems matter.

final class Rejected extends RuntimeError
{
}

$rejected = new Rejected('not today');

$cause = new RuntimeException('the socket closed');
$misconfigured = new LogicException('the host is misconfigured', 7, $cause);

$sandbox = new Sandbox();

$sandbox->registerLibrary('host', [
	'reject' => static function () use ($rejected): never { throw $rejected; },
	'fail' => static function () use ($misconfigured): never { throw $misconfigured; },
]);

// Catchable, and it reads as its message rather than as a userdata address.
var_dump($sandbox->eval('local ok, err = pcall(host.reject) return ok, tostring(err)'));

// Not catchable. pcall does not get a say, and what reaches the host is the
// object the callback threw -- same class, same code, same previous exception --
// rather than a string that used to be one.
try {
	(void) $sandbox->eval('local ok = pcall(host.fail) return "swallowed"', '=contract');
	echo "HOST FAILURE SWALLOWED\n";
} catch (LogicException $error) {
	printf("class=%s same-object=%s code=%d previous-intact=%s\n",
		$error::class, var_export($error === $misconfigured, true), $error->getCode(),
		var_export($error->getPrevious() === $cause, true));
	printf("message=%s is a LuaThrowable=%s\n",
		$error->getMessage(), var_export($error instanceof LuaThrowable, true));
}

// Uncaught, a RuntimeError reaches the host as the same object too, with the
// Lua stack added as context rather than substituted for the exception.
try {
	(void) $sandbox->eval("local function inner() host.reject() end\ninner()", '=contract');
	echo "NOT THROWN\n";
} catch (Rejected $error) {
	printf("same-object=%s chunk=%s line=%s framed=%s\n",
		var_export($error === $rejected, true), var_export($error->getChunkName(), true),
		var_export($error->getLuaLine(), true),
		var_export(count($error->getLuaTrace()) > 0, true));
	var_dump($error->getSandbox() === $sandbox);
}

$sandbox->close();

?>
--EXPECT--
array(2) {
  [0]=>
  bool(false)
  [1]=>
  string(9) "not today"
}
class=LogicException same-object=true code=7 previous-intact=true
message=the host is misconfigured is a LuaThrowable=false
same-object=true chunk='contract' line=1 framed=true
bool(true)
