--TEST--
Nothing a script can read back contains a heap address
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Sandbox;

// Stringification is the primary pointer-disclosure path in the whole library:
// upstream formats tables, functions and userdata as "%s: %p" in luaL_tolstring,
// which backs tostring(), print(), string.format("%s", t), every "attempt to
// index a ..." message and luaL_traceback. Vendored patch 0006 closes it there,
// which is exactly why tostring() is NOT replaced in the library policy -- a
// replacement would be a second stringification path that could only diverge
// from the four the patch already covers.

$sandbox = new Sandbox();

$samples = $sandbox->eval(<<<'LUA'
	local subject = {}
	local closure = function() end

	local _, index_error = pcall(function() local nothing = nil return nothing.field end)
	local _, arith_error = pcall(function() return {} + 1 end)
	local _, format_error = pcall(string.format, "%p", subject)

	local traceback = ""
	if debug ~= nil and debug.traceback ~= nil then
		traceback = tostring(debug.traceback())
	end

	return tostring(subject),
		tostring(closure),
		string.format("%s|%s", subject, closure),
		tostring(index_error),
		tostring(arith_error),
		tostring(format_error),
		traceback
LUA, '=addresses');

foreach ($samples as $index => $sample) {
	$leaks = str_contains($sample, '0x') || preg_match('/[0-9a-f]{8,}/i', $sample) === 1;

	printf("%d %s\n", $index, $leaks ? sprintf('LEAKS: %s', $sample) : 'clean');
}

// Two distinct tables stringify alike, which is the identity the patch gave up
// on purpose. If per-object identity is ever wanted back it belongs in an
// opaque per-sandbox id, not in the address.
var_dump($sandbox->eval('return tostring({}) == tostring({})'));

// And "%p" is refused outright rather than answered with something plausible,
// so a script cannot mistake a sanitised answer for a real one.
var_dump($sandbox->eval('local ok, reason = pcall(string.format, "%p", {}) return ok, reason'));

$sandbox->close();

?>
--EXPECT--
0 clean
1 clean
2 clean
3 clean
4 clean
5 clean
6 clean
array(1) {
  [0]=>
  bool(true)
}
array(2) {
  [0]=>
  bool(false)
  [1]=>
  string(35) "invalid conversion '%p' to 'format'"
}
