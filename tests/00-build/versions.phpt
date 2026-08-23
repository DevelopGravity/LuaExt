--TEST--
Sandbox reports the vendored interpreter version and its own
--EXTENSIONS--
luaext
--FILE--
<?php

use DevelopGravity\LuaExt\Sandbox;

// Pinned by third_party/lua-5.5.1; a vendoring bump must update this test.
var_dump(Sandbox::luaVersion());

// PHP_LUAEXT_VERSION reaches PHP through both the module entry and the method.
var_dump(Sandbox::extensionVersion() === phpversion('luaext'));
var_dump((bool) preg_match('/^\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?$/', Sandbox::extensionVersion()));

?>
--EXPECT--
string(9) "Lua 5.5.1"
bool(true)
bool(true)
