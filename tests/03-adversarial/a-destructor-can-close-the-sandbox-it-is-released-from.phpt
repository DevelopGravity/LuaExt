--TEST--
A host destructor released at the end of a call may close the sandbox that released it
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

/*
 * The deferred queue is drained at the end of an outermost call, which is where
 * releasing a host reference is safe: no Lua execution is in progress. But the
 * release runs a __destruct, and __destruct is arbitrary host code -- with
 * in_lua already back to zero, nothing stops it calling close(), which
 * lua_close()es the interpreter. The frame doing the draining was still holding
 * that lua_State * and still had a stack to settle and results to convert
 * through it.
 *
 * The drain now happens after the last use of the state rather than before it,
 * so a destructor that closes is an ordinary thing to do rather than a
 * use-after-free.
 *
 * Like its neighbour destructor-does-not-run-in-collector.phpt, this asserts
 * ordering and survival rather than a crash: undefined behaviour is free to
 * appear to work, and the real detector is the sanitizer leg. Do not "fix" a
 * failure here by removing that leg.
 */

use DevelopGravity\LuaExt\Sandbox;
use DevelopGravity\LuaExt\SandboxConfig;

class ClosesOnDestruct
{
    public static ?Sandbox $sandbox = null;
    public static array $events = [];

    public function noop(): void
    {
    }

    public function __destruct()
    {
        self::$events[] = 'destructor ran';

        if (self::$sandbox !== null) {
            self::$sandbox->close();
            self::$events[] = 'sandbox closed from destructor';
            self::$sandbox = null;
        }
    }
}

$sandbox = new Sandbox(new SandboxConfig());
ClosesOnDestruct::$sandbox = $sandbox;

// The library holds the only reference to the object. Dropping it here means
// Lua's collector owns its fate: the closure storage's __gc queues the release,
// and the queue drains when this call returns.
$sandbox->registerLibrary('victim', [
    'noop' => [new ClosesOnDestruct(), 'noop'],
]);

// Replace the library so the original closure becomes garbage, collect it, and
// return a real value -- the value the frame must still be able to convert
// through a state a destructor is about to close.
[$answer] = $sandbox->eval(<<<'LUA'
    victim = nil
    collectgarbage("collect")
    collectgarbage("collect")

    return 42
    LUA, '=drop-and-collect');

printf("returned  => %d\n", $answer);

foreach (ClosesOnDestruct::$events as $event) {
    printf("event     => %s\n", $event);
}

// The sandbox really is closed, and says so rather than misbehaving.
try {
    (void) $sandbox->eval('return 1', '=after-close');
    echo "after     => RAN\n";
} catch (Throwable $error) {
    printf("after     => %s\n", substr(strrchr($error::class, '\\'), 1));
}

echo "survived  => yes\n";

?>
--EXPECT--
returned  => 42
event     => destructor ran
event     => sandbox closed from destructor
after     => ClosedSandboxError
survived  => yes
