--TEST--
The luaext module loads and registers its INI settings
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);
$extension = new ReflectionExtension('luaext');

var_dump($extension->getName());

// The module's version and the one the class reports are the same macro.
var_dump($extension->getVersion() === DevelopGravity\LuaExt\Sandbox::extensionVersion());

// The API is entirely class based; the extension declares no plain functions.
var_dump($extension->getFunctions());

$entries = array_keys($extension->getINIEntries());
sort($entries);
var_dump($entries);

?>
--EXPECT--
string(6) "luaext"
bool(true)
array(0) {
}
array(3) {
  [0]=>
  string(17) "luaext.hook_count"
  [1]=>
  string(18) "luaext.use_zend_mm"
  [2]=>
  string(29) "luaext.watchdog_resolution_us"
}
