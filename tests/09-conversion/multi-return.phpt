--TEST--
A Lua multi-return becomes a zero-indexed PHP list, nils included
--EXTENSIONS--
luaext
--XFAIL--
Needs Sandbox::eval()/call(), which land with the execution subsystem.
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Sandbox;

$sandbox = new Sandbox();

// Lua returns any number of values, so every crossing is a list. Zero-indexed
// and positional: a nil in the middle occupies its position rather than being
// dropped, which would shift everything after it.
$results = $sandbox->eval('return 1, nil, "three", 4.5, true, nil');

var_dump(array_is_list($results), count($results), $results);

// No results at all is an empty list, not null.
$none = $sandbox->eval('return');
var_dump(array_is_list($none), $none);

// One result is still a list of one.
var_dump($sandbox->eval('return "only"'));

// Through call(), with arguments converted on the way in.
(void) $sandbox->eval('function pair(a, b) return b, a end');
var_dump($sandbox->call('pair', 'first', 'second'));

// A table among the results converts like any other value.
var_dump($sandbox->eval('return { 10, 20 }, "after"'));

?>
--EXPECT--
bool(true)
int(6)
array(6) {
  [0]=>
  int(1)
  [1]=>
  NULL
  [2]=>
  string(5) "three"
  [3]=>
  float(4.5)
  [4]=>
  bool(true)
  [5]=>
  NULL
}
bool(true)
array(0) {
}
array(1) {
  [0]=>
  string(4) "only"
}
array(2) {
  [0]=>
  string(6) "second"
  [1]=>
  string(5) "first"
}
array(2) {
  [0]=>
  array(2) {
    [1]=>
    int(10)
    [2]=>
    int(20)
  }
  [1]=>
  string(5) "after"
}
