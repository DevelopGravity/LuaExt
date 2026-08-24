--TEST--
A host exception crosses Lua and comes back as the same object
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Exception\LuaThrowable;
use DevelopGravity\LuaExt\Exception\RuntimeError;
use DevelopGravity\LuaExt\Sandbox;

// The old extension kept only the message of an exception a callback threw, so
// a host could no longer tell a database outage from a validation failure by
// catching a class. Here the object itself is retained on the error value and
// rethrown, which is also what stops a script deciding that someone else's
// failure is its to handle.

final class BackendUnavailable extends RuntimeException
{
	public function __construct(string $message, public readonly string $host) {
		parent::__construct($message, 42);
	}
}

$outage = new BackendUnavailable('the backend is unavailable', 'redis-7');

$sandbox = new Sandbox();
$sandbox->registerLibrary('host', [
	'crash' => static function () use ($outage): never { throw $outage; },
	'reject' => static function (): never { throw new RuntimeError('handle me'); },
]);

// Not a RuntimeError, so it aborts the script -- and arrives with its identity,
// its class, its code and its own properties intact.
try {
	(void) $sandbox->eval('host.crash()', '=roundtrip');
	echo "NOT THROWN\n";
} catch (Throwable $error) {
	printf("class=%s same-object=%s code=%d host=%s message=%s\n",
		$error::class, var_export($error === $outage, true), $error->getCode(),
		$error->host, $error->getMessage());
	printf("is a LuaThrowable=%s\n", var_export($error instanceof LuaThrowable, true));
}

// The script could not have caught it on the way out.
try {
	$result = $sandbox->eval('local ok = pcall(host.crash) return "swallowed"', '=roundtrip');
	printf("HOST FAILURE SWALLOWED: %s\n", var_export($result, true));
} catch (BackendUnavailable $error) {
	printf("pcall could not swallow it: %s\n", $error->getMessage());
}

// A RuntimeError is the host saying the script is meant to deal with this, so
// it stays catchable and reads as its message.
var_dump($sandbox->eval('local ok, err = pcall(host.reject) return ok, tostring(err)', '=roundtrip'));

// Uncaught, the same RuntimeError comes back to the host with the Lua stack
// attached -- the traceback is context added to the exception, not a
// replacement for it.
try {
	(void) $sandbox->eval("local function inner() host.reject() end\ninner()", '=context');
} catch (RuntimeError $error) {
	printf("chunk=%s line=%s frames=%d innermost=%s outermost=%s\n",
		var_export($error->getChunkName(), true), var_export($error->getLuaLine(), true),
		count($error->getLuaTrace()),
		$error->getLuaTrace()[0]['what'],
		$error->getLuaTrace()[count($error->getLuaTrace()) - 1]['what']);
	var_dump($error->getSandbox() === $sandbox);
}

?>
--EXPECT--
class=BackendUnavailable same-object=true code=42 host=redis-7 message=the backend is unavailable
is a LuaThrowable=false
pcall could not swallow it: the backend is unavailable
array(2) {
  [0]=>
  bool(false)
  [1]=>
  string(9) "handle me"
}
chunk='context' line=1 frames=3 innermost=C outermost=main
bool(true)
