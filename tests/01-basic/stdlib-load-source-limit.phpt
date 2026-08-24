--TEST--
load() honours maxSourceBytes and refuses bytecode without loadBytecode
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Capabilities;
use DevelopGravity\LuaExt\Limits;
use DevelopGravity\LuaExt\Sandbox;
use DevelopGravity\LuaExt\SandboxConfig;

// Sandbox::compile() enforces maxSourceBytes and upstream's load() does not, so
// compileAtRuntime would otherwise open a straight bypass of it. The limit is
// measured in bytes rather than in CPU because parsing is the one phase no
// interrupt can land in: the hook that stops a runaway script only runs between
// the instructions of a chunk that already compiled.

$sandbox = new Sandbox(new SandboxConfig(
	capabilities: (new Capabilities())->with(compileAtRuntime: true),
	limits: new Limits(maxSourceBytes: 4096),
));

// A chunk that fits compiles and runs.
var_dump($sandbox->eval('return load("return 1 + 1")()'));

// One that does not is refused as load()'s own documented failure -- fail plus
// a message -- rather than as a syntax error about the source itself.
var_dump($sandbox->eval(
	'local chunk, reason = load(string.rep("-", 5000)) return chunk == nil, tostring(reason)',
));

// The reader-function form is capped on what it accumulates, which is the only
// way to bound it: there is no string to measure before the parser starts.
// Returning nil at the cap would silently compile whatever prefix had already
// been fed in, so it refuses instead.
var_dump($sandbox->eval(<<<'LUA'
	local calls = 0
	local chunk, reason = load(function()
		calls = calls + 1
		if calls > 400 then return nil end
		return string.rep("-", 20)
	end)

	return chunk == nil, tostring(reason)
LUA));

// A reader that stays under the cap is untouched.
var_dump($sandbox->eval(<<<'LUA'
	local parts = {"return ", "40", " + 2"}
	local index = 0

	return load(function() index = index + 1 return parts[index] end)()
LUA));

// The mode is forced to "t" without loadBytecode: Lua has no bytecode verifier,
// so a binary chunk is not a parse away from native execution, it IS native
// execution. Asking for "b" explicitly does not change that.
var_dump($sandbox->eval('local chunk, reason = load("\27Lua", "=c", "b") return chunk == nil, tostring(reason)'));

$sandbox->close();

?>
--EXPECT--
array(1) {
  [0]=>
  int(2)
}
array(2) {
  [0]=>
  bool(true)
  [1]=>
  string(98) "the chunk is 5000 bytes, which exceeds the 4096 byte source limit this sandbox was configured with"
}
array(2) {
  [0]=>
  bool(true)
  [1]=>
  string(77) "the chunk exceeds the 4096 byte source limit this sandbox was configured with"
}
array(1) {
  [0]=>
  int(42)
}
array(2) {
  [0]=>
  bool(true)
  [1]=>
  string(44) "attempt to load a binary chunk (mode is 't')"
}
