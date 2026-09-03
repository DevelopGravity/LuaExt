--TEST--
A script cannot reach the binary loader itself, even holding loadBytecode
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Capabilities;
use DevelopGravity\LuaExt\Sandbox;
use DevelopGravity\LuaExt\SandboxConfig;

// There are TWO doors into Lua's unvalidated binary loader, and gating only the
// host's would leave the other open:
//
//   1. $sandbox->compileBinary($bytes)      -- the host
//   2. load($bytes, "=c", "b") inside Lua   -- the SCRIPT
//
// The second is the one that is easy to forget. A script granted loadBytecode
// can assemble bytes itself -- string.char(), a VFS read, a host callback's
// return value -- and hand them to the same loader that dies on a flipped byte.
//
// A script can never seal anything, because it never sees the key. So with
// luaext.allow_raw_bytecode off there is no script-side path to the binary
// loader at all. That is the intent, not a side effect.

$sandbox = new Sandbox(new SandboxConfig(
	capabilities: (new Capabilities())->with(
		compileAtRuntime: true,
		loadBytecode: true,
		dumpBytecode: true,
	),
));

// Real bytecode, of the kind a script could plausibly get hold of.
$sandbox->setGlobal('BYTES', $sandbox->eval('return string.dump(function() return 42 end)')[0]);

$probe = <<<'LUA'
	local chunk, reason = load(BYTES, "=c", "b")
	return chunk == nil, tostring(reason)
LUA;

[$refused, $reason] = $sandbox->eval($probe, '@probe.lua');

printf("script's load(..., \"b\") refused: %s\n", var_export($refused, true));
printf("and it says why: %s\n", var_export(str_contains($reason, 'binary'), true));

// Text mode is untouched: compileAtRuntime is a real capability and this must
// not have quietly disabled it.
var_dump($sandbox->eval('local f = load("return 7") return f()', '@text.lua'));

// The capability still gates the HOST side, which is a separate question from
// whether the deployment permits raw bytecode at all.
printf("host still refuses raw: %s\n", var_export(
	(static function (Sandbox $sandbox): bool {
		try {
			$sandbox->compileBinary($sandbox->getGlobal('BYTES'), '@host.lua');

			return false;
		} catch (Throwable $error) {
			return true;
		}
	})($sandbox),
	true,
));

$sandbox->close();

?>
--EXPECT--
script's load(..., "b") refused: true
and it says why: true
array(1) {
  [0]=>
  int(7)
}
host still refuses raw: true
