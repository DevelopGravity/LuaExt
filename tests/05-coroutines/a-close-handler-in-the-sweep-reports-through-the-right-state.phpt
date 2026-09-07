--TEST--
A <close> handler swept at end-of-call runs as the coroutine, and a sink exception it provokes survives
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\OutputMode;
use DevelopGravity\LuaExt\Sandbox;
use DevelopGravity\LuaExt\SandboxConfig;

// The end-of-call sweep closes every coroutine the script left suspended, and
// closing runs their <close> handlers -- untrusted script code -- on the swept
// thread. A handler that prints into a Callback sink whose callback throws
// used to route the raise through the MAIN thread, whose protected call had
// already returned: the raise reached the panic handler and took the whole
// request as an uncatchable E_ERROR. The handler must run as the coroutine,
// and the host's own exception must come back to the host, not vanish into
// the sweep.

$sandbox = new Sandbox(new SandboxConfig(
	outputMode: OutputMode::Callback,
	outputChunkBytes: 0,
	outputCallback: static function (string $chunk, bool $isStderr): void {
		throw new DomainException('sink refused: ' . trim($chunk));
	},
));

// The body itself prints nothing, so the sink is reached ONLY from the sweep:
// the coroutine parks a pending <close> and stays suspended past the return.
try {
	(void) $sandbox->eval(<<<'LUA'
		local co = coroutine.create(function()
			local guard <close> = setmetatable({}, {
				__close = function() print("from the close handler") end,
			})
			coroutine.yield()
		end)
		coroutine.resume(co)
		return "done"
	LUA, '=sweep');
	print "eval     returned\n";
} catch (DomainException $error) {
	printf("eval     threw %s: %s\n", $error::class, $error->getMessage());
}

// The sweep still did its whole job -- nothing stayed live -- and the sandbox
// is not poisoned: the next call runs normally.
printf("live     %d\n", $sandbox->stats()->liveCoroutines);
printf("next     %s\n", $sandbox->eval('return "still works"', '=next')[0]);

$sandbox->close();

?>
--EXPECT--
eval     threw DomainException: sink refused: from the close handler
live     0
next     still works
