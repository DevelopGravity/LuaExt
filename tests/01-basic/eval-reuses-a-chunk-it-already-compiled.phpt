--TEST--
eval()'s compile cache is invisible except in timing, and stays inside its bound
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Limits;
use DevelopGravity\LuaExt\Sandbox;
use DevelopGravity\LuaExt\SandboxConfig;

// eval() reparses its source on every call. With the cache on, the compiled
// chunk is kept and reused -- which is only ever a speed change. THE FAILURE
// MODE OF A CACHE IS A WRONG ANSWER, not a crash, so the first and most
// important thing here is that the two configurations agree exactly.

$cached = new Sandbox(new SandboxConfig(cacheCompiledChunks: true));
$plain = new Sandbox();

$scripts = [
	'return 1 + 1',
	'local t = {} for i = 1, 5 do t[i] = i * i end return t[3]',
	'return type(nil)',
	'local s = "" for i = 1, 3 do s = s .. i end return s',
	'return select("#", 1, 2, 3)',
	'local a, b = 1, 2 return a + b, a * b',
];

$agree = true;

foreach ($scripts as $source) {
	// Three rounds each: the first populates, the rest hit.
	for ($round = 0; $round < 3; $round++) {
		if ($cached->eval($source, '@agree.lua') !== $plain->eval($source, '@agree.lua')) {
			$agree = false;
		}
	}
}

printf("identical with cache on and off: %s\n", var_export($agree, true));
printf("entries held: cached=%d plain=%d\n",
	$cached->stats()->cachedChunks, $plain->stats()->cachedChunks);

// A main chunk's only upvalue is _ENV, so a reused chunk reads whatever the
// globals hold NOW. A cache that froze them would be the subtlest possible bug.
$cached->setGlobal('G', 1);
printf("global before: %d\n", $cached->eval('return G', '@env.lua')[0]);
$cached->setGlobal('G', 99);
printf("global after:  %d\n", $cached->eval('return G', '@env.lua')[0]);

// Top-level locals are per-call state, exactly as they are for a compiled
// LuaFunction called repeatedly. Reusing the chunk must not make them persist.
$counts = [];

for ($call = 0; $call < 3; $call++) {
	$counts[] = $cached->eval('local n = (n or 0) + 1 return n', '@locals.lua')[0];
}

printf("locals do not persist: %s\n", implode(',', $counts));

// The chunk name is part of the key, because it decides what every traceback
// out of that chunk says. Identical source under two names is two chunks.
$before = $cached->stats()->cachedChunks;
(void) $cached->eval('return 7', '@one.lua');
(void) $cached->eval('return 7', '@two.lua');
printf("one source, two names: +%d entries\n", $cached->stats()->cachedChunks - $before);

// And the traceback still names the right one.
try {
	(void) $cached->eval('error("boom")', '@named.lua');
} catch (Throwable $error) {
	printf("traceback names: %s\n", var_export($error->getChunkName(), true));
}

$cached->close();
$plain->close();

// Past the ceiling, evaluation keeps working and simply stops being cached. A
// full cache making a working call fail would be a far worse trade than a slow
// one, so this asserts results as well as the count.
$bounded = new Sandbox(new SandboxConfig(
	limits: new Limits(maxCachedChunks: 3),
	cacheCompiledChunks: true,
));

$allCorrect = true;

for ($index = 0; $index < 10; $index++) {
	if ($bounded->eval("return {$index}", "@bound{$index}.lua")[0] !== $index) {
		$allCorrect = false;
	}
}

printf("cap 3, ten chunks: held=%d, results correct=%s\n",
	$bounded->stats()->cachedChunks, var_export($allCorrect, true));

// Re-evaluating one that IS cached still works after the cache filled.
printf("cached entry after fill: %d\n", $bounded->eval('return 0', '@bound0.lua')[0]);
$bounded->close();

// Off by default. Enabling it is what changes behaviour, so a caller who never
// heard of this feature cannot be surprised by its memory.
$default = new Sandbox();
(void) $default->eval('return 1', '@default.lua');
(void) $default->eval('return 1', '@default.lua');
printf("off by default: %d\n", $default->stats()->cachedChunks);
$default->close();

// A syntax error is not cached, and does not consume an entry.
$failing = new Sandbox(new SandboxConfig(cacheCompiledChunks: true));

for ($attempt = 0; $attempt < 2; $attempt++) {
	try {
		(void) $failing->eval('return ((', '@broken.lua');
	} catch (Throwable $error) {
	}
}

printf("a chunk that will not parse: %d entries\n", $failing->stats()->cachedChunks);
printf("and the sandbox still works: %d\n", $failing->eval('return 6 * 7', '@after.lua')[0]);
$failing->close();

?>
--EXPECT--
identical with cache on and off: true
entries held: cached=6 plain=0
global before: 1
global after:  99
locals do not persist: 1,1,1
one source, two names: +2 entries
traceback names: 'named.lua'
cap 3, ten chunks: held=3, results correct=true
cached entry after fill: 0
off by default: 0
a chunk that will not parse: 0 entries
and the sandbox still works: 42
