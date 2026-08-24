--TEST--
math.randomseed returns nothing, and the generator is seeded by the sandbox
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Sandbox;
use DevelopGravity\LuaExt\SandboxConfig;

// Two separate leaks, and replacing the function only closes one of them.
//
// Upstream's math.randomseed RETURNS the components it derived, and with no
// argument it derives them from luaL_makeseed() -- a stack address mixed with a
// second-granularity clock. Returning them hands a script the address.
//
// The other half is that the SAME luaL_makeseed seeds the generator when the
// library opens, so a script that calls math.random(0) a few times can recover
// the state offline and work backwards to the address. Only reseeding fixes
// that, which is what a sandbox does at construction.

$draw = 'local out = {} for index = 1, 6 do out[index] = math.random(1, 1000000) end
	return table.concat(out, ",")';

$sandbox = new Sandbox();

// Void: not one return value, let alone two address-derived ones.
var_dump($sandbox->eval('return select("#", math.randomseed(1))'));

// Integer-only. The no-argument form is what reseeds from an address.
var_dump($sandbox->eval('return pcall(math.randomseed)')[0]);
var_dump($sandbox->eval('return pcall(math.randomseed, 1.5)')[0]);

// It still seeds: the same seed twice gives the same draw.
var_dump($sandbox->eval(
	'math.randomseed(99) local first = math.random(1, 1000000)
	math.randomseed(99) return first == math.random(1, 1000000)',
));

// And math.random works untouched after the reseed at construction.
var_dump($sandbox->eval('local value = math.random(1, 10) return value >= 1 and value <= 10'));

$sandbox->close();

// A host that pinned the seed gets the same sequence from two sandboxes, which
// is what deterministic: true promises.
$fixed = static fn (): Sandbox => new Sandbox(new SandboxConfig(seed: 20260824, deterministic: true));

var_dump($fixed()->eval($draw)[0] === $fixed()->eval($draw)[0]);

// A host that did not gets a different one, drawn from the CSPRNG rather than
// from where the C stack happened to be.
var_dump((new Sandbox())->eval($draw)[0] === (new Sandbox())->eval($draw)[0]);

?>
--EXPECT--
array(1) {
  [0]=>
  int(0)
}
bool(false)
bool(false)
array(1) {
  [0]=>
  bool(true)
}
array(1) {
  [0]=>
  bool(true)
}
bool(true)
bool(false)
