--TEST--
A <close> handler cannot smuggle suspended coroutines past the call-scope sweep
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Sandbox;
use DevelopGravity\LuaExt\SandboxConfig;

// The call-scope sweep walks the coroutine tracking table and closes every
// thread, and closing a thread runs its <close> handlers -- script code. A
// handler that created coroutines mid-sweep inserted into the very table
// lua_next was walking; the rehash skipped entries, and a skipped suspended
// coroutine survived into the next call, resumable, while liveCoroutines read
// zero. Measured before the fix: 45 of 50 iterations of this exact shape
// leaked one. Run in a loop, because a single iteration can pass on rehash
// luck.
//
// The sweep now detaches the table before walking it and refuses coroutine
// creation for its duration -- the call is over, and new suspended state at
// teardown is precisely what the sweep exists to end.

$sandbox = new Sandbox(new SandboxConfig());

$escaped = 0;
$statsWrong = 0;

for ($iteration = 0; $iteration < 50; $iteration++) {
	(void) $sandbox->eval(<<<'LUA'
		stash = stash or {}

		-- Ordinary suspended coroutines the sweep must close.
		for i = 1, 8 do
			local co = coroutine.create(function()
				coroutine.yield()
			end)
			coroutine.resume(co)
			stash[#stash + 1] = co
		end

		-- One whose <close> handler tries to mint more, mid-sweep.
		local trouble = coroutine.create(function()
			local guard <close> = setmetatable({}, {
				__close = function()
					for i = 1, 8 do
						local extra = coroutine.create(function()
							coroutine.yield()
						end)
						coroutine.resume(extra)
						stash[#stash + 1] = extra
					end
				end,
			})
			coroutine.yield()
		end)
		coroutine.resume(trouble)

		return true
		LUA, '=make-trouble');

	if ($sandbox->stats()->liveCoroutines !== 0) {
		$statsWrong++;
	}

	[$resumable] = $sandbox->eval(<<<'LUA'
		local count = 0

		for _, co in ipairs(stash or {}) do
			if coroutine.status(co) == "suspended" then
				count = count + 1
			end
		end

		stash = {}
		return count
		LUA, '=count-survivors');

	if ($resumable > 0) {
		$escaped++;
	}
}

printf("escaped    => %d of 50\n", $escaped);
printf("stats      => %s\n", $statsWrong === 0 ? 'honest every time' : sprintf('WRONG %d times', $statsWrong));

// Coroutines created normally, in a live call, still work exactly as before.
[$sum] = $sandbox->eval(<<<'LUA'
	local co = coroutine.wrap(function()
		coroutine.yield(20)
		coroutine.yield(22)
	end)

	return co() + co()
	LUA, '=still-working');

printf("normal use => %d\n", $sum);

$sandbox->close();

?>
--EXPECT--
escaped    => 0 of 50
stats      => honest every time
normal use => 42
