--TEST--
Sandbox::interrupt() stops a script and cannot be swallowed
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Exception\CpuLimitError;
use DevelopGravity\LuaExt\Exception\HostAbortError;
use DevelopGravity\LuaExt\Limits;
use DevelopGravity\LuaExt\Sandbox;
use DevelopGravity\LuaExt\SandboxConfig;

/*
 * interrupt() is the one method a foreign thread may call, so it does two
 * atomic stores and nothing else -- no lock, no allocation, and nothing that
 * reads a field the owning thread writes unsynchronised. It cannot raise the
 * error itself, because it is not on the thread that would have to unwind: it
 * asks, and the next hook tick answers.
 *
 * A PHP callback is the only way to reach a running sandbox from a single
 * thread, so that is where these ask from. What is being tested is the delivery
 * path, and that path is identical whichever thread set the flag.
 */

const WALL_BACKSTOP_SECONDS = 2.0;

function bounded(): Sandbox
{
	return new Sandbox(new SandboxConfig(
		limits: (new Limits())->with(
			// Generous, so that anything that stops below is the interrupt and
			// not the budget quietly running out first.
			cpuSeconds: 5.0,
			wallClockSeconds: WALL_BACKSTOP_SECONDS,
		),
	));
}

$sandbox = bounded();

$sandbox->registerLibrary('host', [
	'abort' => static function () use ($sandbox): void {
		$sandbox->interrupt();
	},
]);

try {
	(void) $sandbox->eval('host.abort() while true do end', '=aborted');
	echo "NOT STOPPED\n";
} catch (HostAbortError) {
	echo "interrupted\n";
} catch (Throwable $error) {
	printf("WRONG CLASS %s\n", $error::class);
}

// An interrupt is not a limit: nothing was consumed on the sandbox's behalf, so
// the sandbox stays usable and its budget stays where it was. The flag is
// dropped when the outermost call unwinds, or every later call would inherit an
// abort that had already been reported.
var_dump($sandbox->isClosed());
var_dump($sandbox->eval('return "still alive"', '=after'));

$sandbox->close();

/*
 * The delivery path is the same one the limits use, so it has to be just as
 * hard to swallow. An interrupt caught by a pcall and ignored still stops the
 * script, because the flag stays raised until the call that it aborted has
 * finished unwinding.
 */
$stubborn = bounded();

$stubborn->registerLibrary('host', [
	'abort' => static function () use ($stubborn): void {
		$stubborn->interrupt();
	},
]);

try {
	(void) $stubborn->eval(<<<'LUA'
		pcall(function()
			host.abort()

			while true do end
		end)

		return "swallowed"
	LUA, '=swallower');

	echo "SWALLOWED\n";
} catch (HostAbortError) {
	echo "cannot be swallowed\n";
} catch (Throwable $error) {
	printf("WRONG CLASS %s\n", $error::class);
}

$stubborn->close();

/*
 * An interrupt raised on a sandbox that has NO limit at all still has to be
 * dropped when the call ends. Nothing arms the watchdog for such a sandbox, so
 * if clearing the flag were the watchdog's job it would never happen and every
 * later call would fail with a stale abort.
 */
$unbounded = new Sandbox(new SandboxConfig(
	limits: (new Limits())->with(cpuSeconds: null, wallClockSeconds: null),
));

$unbounded->registerLibrary('host', [
	'abort' => static function () use ($unbounded): void {
		$unbounded->interrupt();
	},
]);

try {
	(void) $unbounded->eval('host.abort() while true do end', '=unbounded');
	echo "NOT STOPPED\n";
} catch (HostAbortError) {
	echo "interrupted without any limit\n";
} catch (CpuLimitError) {
	echo "WRONG CLASS: something armed a limit that was never asked for\n";
}

var_dump($unbounded->eval('return "not poisoned"', '=after'));

$unbounded->close();

?>
--EXPECT--
interrupted
bool(false)
array(1) {
  [0]=>
  string(11) "still alive"
}
cannot be swallowed
interrupted without any limit
array(1) {
  [0]=>
  string(12) "not poisoned"
}
