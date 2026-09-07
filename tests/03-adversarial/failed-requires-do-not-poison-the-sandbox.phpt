--TEST--
A module that fails to compile does not strand require()'s bookkeeping
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Capabilities;
use DevelopGravity\LuaExt\Exception\LuaThrowable;
use DevelopGravity\LuaExt\Limits;
use DevelopGravity\LuaExt\ModuleResolver;
use DevelopGravity\LuaExt\ModuleSource;
use DevelopGravity\LuaExt\Sandbox;
use DevelopGravity\LuaExt\SandboxConfig;

// require() marks a module in-progress and counts a nesting level before its
// loaders run, and a loader that raises -- a module that does not compile --
// longjmps straight past the frame that set both. Unfixed, every such failure
// leaks one level of require_depth and one permanent in-progress mark: the
// same module then reports "requires itself", and maxRequireDepth failures
// later require() is dead for the sandbox's life behind an uncatchable depth
// error. The searches run protected now, so the bookkeeping unwinds on every
// branch.

$resolver = new class implements ModuleResolver {
	public function resolve(string $name, string $requestedBy): ?ModuleSource
	{
		// Every "bad.*" name resolves to source that cannot compile; anything
		// else is not provided at all.
		if (str_starts_with($name, 'bad')) {
			return new ModuleSource('this is not lua (', '=' . $name);
		}

		return null;
	}
};

$sandbox = new Sandbox(new SandboxConfig(
	capabilities: (new Capabilities())->with(require: true),
	limits: new Limits(maxRequireDepth: 8),
	moduleResolver: $resolver,
));

// Twice the depth budget in failed requires. Each is caught in-script -- a
// compile failure is the script's mistake to handle -- and each must cost
// nothing durable.
[$failures, $lastMessage] = $sandbox->eval(<<<'LUA'
	local failures = 0
	local last

	for index = 1, 16 do
		local ok, err = pcall(require, "bad" .. index)

		if not ok then
			failures = failures + 1
			last = tostring(err)
		end
	end

	return failures, last
	LUA, '=burn-failures');

printf("failures    => %d\n", $failures);
printf("last error  => %s\n",
	str_contains($lastMessage, 'maxRequireDepth') ? 'DEPTH ERROR (leaked)' : 'compile error');

// The bookkeeping survived: an unresolvable name still reports plain
// not-found, catchably.
[$ok, $notFound] = $sandbox->eval(<<<'LUA'
	local ok, err = pcall(require, "no.such.module")
	return ok, tostring(err)
	LUA, '=still-alive');

printf("not-found   => %s\n", $notFound);

// And retrying a module that failed reports the same compile failure -- never
// "requires itself", which is what a leaked in-progress mark turns it into.
[$retry] = $sandbox->eval(<<<'LUA'
	local ok, err = pcall(require, "bad1")
	return tostring(err)
	LUA, '=retry');

printf("retry       => %s\n",
	str_contains($retry, 'requires itself') ? 'REQUIRES ITSELF (leaked mark)' : 'compile error again');

// A module that loads normally still does, in the same sandbox.
$sandbox->preloadModule('fine', static fn (): int => 42);
[$fine] = $sandbox->eval('return require("fine")', '=fine');
printf("healthy     => %d\n", $fine);

$sandbox->close();

?>
--EXPECT--
failures    => 16
last error  => compile error
not-found   => Module "no.such.module" was not found
retry       => compile error again
healthy     => 42
