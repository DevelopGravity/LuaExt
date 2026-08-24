--TEST--
Conversion walks tables raw and never runs a metamethod
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Sandbox;

$sandbox = new Sandbox();

// Converting a returned value must not hand control back to the script. A
// metamethod running here would execute untrusted code after the call has
// nominally finished, outside the pcall that was protecting it, and could
// allocate, recurse or simply never return.
//
// So the walk is raw: only the keys the table itself holds, in whatever order
// lua_next produces them.
[$converted] = $sandbox->eval(<<<'LUA'
	return setmetatable({ real = 1 }, {
		__index = function() return "inherited" end,
		__pairs = function() error("__pairs ran during conversion") end,
		__len = function() return 1000 end,
		__tostring = function() error("__tostring ran during conversion") end,
	})
	LUA);

var_dump($converted);

// The metatable itself is not part of the table's contents and does not appear.
var_dump(array_keys($converted));

// The script's own view is unchanged: the metamethods are still installed and
// still work when Lua asks for them.
var_dump($sandbox->eval(<<<'LUA'
	local proxy = setmetatable({ real = 1 }, { __index = function() return "inherited" end })
	return proxy.real, proxy.missing
	LUA));

?>
--EXPECT--
array(1) {
  ["real"]=>
  int(1)
}
array(1) {
  [0]=>
  string(4) "real"
}
array(2) {
  [0]=>
  int(1)
  [1]=>
  string(9) "inherited"
}
