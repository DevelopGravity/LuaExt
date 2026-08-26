--TEST--
A host destructor released by a Lua finaliser does not run inside the collector
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

/*
 * registerLibrary() hands the closure storage a reference to whatever the
 * callable is bound to. When Lua collects that storage its __gc releases the
 * reference -- and if it was the last one, that ran the bound object's
 * __destruct from inside the collector of the very state the destructor is free
 * to call back into. Re-entering a lua_State from its own GC is undefined
 * behaviour: the collector is walking structures the re-entrant call may mutate.
 *
 * The references are now handed to a deferred queue and released where no Lua
 * execution is in progress.
 *
 * WHAT THIS TEST MEASURES, and why it is ordering rather than a crash: undefined
 * behaviour is free to appear to work, and on this platform it does -- an
 * earlier version of this file passed with and without the fix, which made it
 * worthless. The one thing that is deterministic either way is WHEN the
 * destructor runs relative to the script that triggered it. Inside the
 * collector, it runs in the middle of collectgarbage(); deferred, it runs only
 * after the script has finished. The Lua-side sequence below records that
 * directly.
 */

use DevelopGravity\LuaExt\Sandbox;

class Reentrant
{
    public static ?Sandbox $sandbox = null;

    public function noop(): void
    {
    }

    public function __destruct()
    {
        if (self::$sandbox === null || self::$sandbox->isClosed()) {
            return;
        }

        // The dangerous call: back into the sandbox whose finaliser released
        // this object. It appends to the same table the script writes to, so
        // the resulting order says where it ran.
        (void) self::$sandbox->eval('order[#order + 1] = "destructor"', '=fromdtor');
    }
}

$sandbox = new Sandbox();
Reentrant::$sandbox = $sandbox;

$sandbox->registerLibrary('svc', ['go' => [new Reentrant(), 'noop']]);

$order = $sandbox->eval(<<<'LUA'
	order = {}

	-- Drop the only reference and collect it. The finaliser runs somewhere in
	-- here; the question is whether the destructor it triggers runs here too.
	svc = nil
	collectgarbage("collect")
	collectgarbage("collect")

	order[#order + 1] = "script"

	return table.concat(order, ",")
LUA, '=drop');

/*
 * "script" -- the destructor had not run by the time the script computed its
 * return value, which is the property under test. The queue drains after the
 * call unwinds, so its entry cannot appear here.
 *
 * "destructor,script" is the bug: the destructor re-entered while
 * collectgarbage() was still sweeping, ahead of the script that triggered it.
 */
var_dump($order[0]);

// ...and it did run, just later. Reading the table again shows the entry that
// arrived after the previous call returned -- deferred, not dropped.
$after = $sandbox->eval('return table.concat(order, ",")', '=recheck');

var_dump($after[0]);

$sandbox->close();
Reentrant::$sandbox = null;

echo "survived\n";

?>
--EXPECT--
string(6) "script"
string(17) "script,destructor"
survived
