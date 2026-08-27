--TEST--
LuaMethod cannot be applied to anything but a method
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);
#[DevelopGravity\LuaExt\LuaMethod]
class Host
{
}

?>
--EXPECTF--
Fatal error: Attribute "DevelopGravity\LuaExt\LuaMethod" cannot target class (allowed targets: method) in %s on line %d
