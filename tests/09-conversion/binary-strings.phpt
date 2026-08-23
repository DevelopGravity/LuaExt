--TEST--
Strings cross the boundary as bytes, NUL bytes included
--EXTENSIONS--
luaext
--XFAIL--
Needs Sandbox::eval()/setGlobal()/getGlobal(), which land with the execution subsystem.
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Sandbox;

// Both languages hold strings as counted byte arrays, so a NUL in the middle
// of one is content. Anything that treats it as a terminator truncates data
// silently, which is why the length is asserted alongside the bytes.
$blob = "head\x00middle\xff\x01\x7ftail";

$sandbox = new Sandbox();
$sandbox->setGlobal('blob', $blob);

var_dump(strlen($blob));
var_dump($sandbox->getGlobal('blob') === $blob);
var_dump($sandbox->eval('return #blob'));

// Lua's own view of the bytes, so the check does not merely compare a copy of
// the string with itself.
var_dump($sandbox->eval('return blob:byte(5), blob:byte(12), blob:sub(1, 4)'));

// Lua -> PHP for a string Lua built, including a trailing NUL where a
// terminator-based copy would lose the final byte.
[$built] = $sandbox->eval('return "a\0b\0"');
var_dump(strlen($built), bin2hex($built));

// A NUL byte inside a table key survives too, and does not make the key look
// numeric to PHP: "1\0" is a string key, not the integer 1.
[$keyed] = $sandbox->eval('return { ["1\0"] = "kept" }');
var_dump(array_map('bin2hex', array_keys($keyed)), $keyed["1\x00"]);

?>
--EXPECT--
int(18)
bool(true)
array(1) {
  [0]=>
  int(18)
}
array(3) {
  [0]=>
  int(0)
  [1]=>
  int(255)
  [2]=>
  string(4) "head"
}
int(4)
string(8) "61006200"
array(1) {
  [0]=>
  string(4) "3100"
}
string(4) "kept"
