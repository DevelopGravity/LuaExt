--TEST--
close() is refused while the sandbox is running rather than freeing the running state
--EXTENSIONS--
luaext
--FILE--
<?php

use DevelopGravity\LuaExt\Exception\ConfigurationError;
use DevelopGravity\LuaExt\Sandbox;

$sandbox = new Sandbox();

/*
 * A host callback can always reach the sandbox running it -- here by capture,
 * which is exactly what registerObject() does more explicitly. Closing from
 * inside would run lua_close() on the state whose frame we are standing in.
 */
$sandbox->registerLibrary('host', [
    'shutdown' => function () use ($sandbox): string {
        try {
            $sandbox->close();

            return 'closed';
        } catch (ConfigurationError $error) {
            return 'refused';
        }
    },
]);

[$outcome] = $sandbox->eval('return host.shutdown()');
var_dump($outcome);

// The refusal must leave the sandbox intact, not half torn down.
var_dump($sandbox->isClosed());
[$stillWorks] = $sandbox->eval('return 6 * 7');
var_dump($stillWorks);

// And closing from outside a call still works normally.
$sandbox->close();
var_dump($sandbox->isClosed());

?>
--EXPECT--
string(7) "refused"
bool(false)
int(42)
bool(true)
