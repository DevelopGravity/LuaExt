--TEST--
xpcall's message handler never runs for a fatal, so it cannot launder the marker
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Sandbox;

// A message handler's return value BECOMES the error object. So a handler that
// is allowed to run for a fatal can replace the unforgeable marker with a plain
// string, and every protected call further out then treats a limit breach or a
// host failure as an ordinary catchable error.
//
// `xpcall(f, function() return "oops" end)` is the entire attack, and it is one
// line of Lua. Running the handler and rethrowing afterwards does not help: by
// then the substitution has already happened. The handler is skipped.

$sandbox = new Sandbox();
$sandbox->registerLibrary('host', [
	'fail' => static function (): never { throw new LogicException('the host broke'); },
]);

try {
	$result = $sandbox->eval(<<<'LUA'
		local ok, err = xpcall(host.fail, function(error_value)
			handler_ran = true
			return "laundered"
		end)

		return "swallowed", ok, err
	LUA, '=launder');

	printf("ESCAPED: %s\n", var_export($result, true));
} catch (Throwable $error) {
	printf("class=%s message=%s\n", $error::class, $error->getMessage());
}

// Both halves matter. The handler never ran, and what reached the host is still
// the typed exception the callback threw rather than the string the handler
// wanted to put in its place.
var_dump($sandbox->getGlobal('handler_ran'));

$sandbox->close();

// A handler nested inside another xpcall's handler gets no second chance
// either: the fatal passes through every one of them untouched.
$sandbox = new Sandbox();
$sandbox->registerLibrary('host', [
	'fail' => static function (): never { throw new LogicException('the host broke'); },
]);

try {
	(void) $sandbox->eval(<<<'LUA'
		xpcall(function()
			xpcall(host.fail, function() inner_ran = true return "inner" end)
		end, function() outer_ran = true return "outer" end)

		return "swallowed"
	LUA, '=launder');

	echo "ESCAPED\n";
} catch (LogicException $error) {
	printf("nested: stopped, %s\n", $error->getMessage());
}

var_dump($sandbox->getGlobal('inner_ran'), $sandbox->getGlobal('outer_ran'));

$sandbox->close();

// For an ordinary script error the handler runs and its answer is the error, so
// xpcall still does what it is for.
$sandbox = new Sandbox();

var_dump($sandbox->eval(
	'return xpcall(function() error("mine") end, function(err) return "handled: " .. err end)',
	'=ordinary',
));

// Including the case the handler itself fails, which upstream answers with
// "error in error handling" and which is not a fatal: it can only arise from
// the script's own handler failing on an error the script was allowed to catch.
var_dump($sandbox->eval(
	'return xpcall(function() error("mine") end, function() error("and again") end)',
	'=ordinary',
));

$sandbox->close();

?>
--EXPECT--
class=LogicException message=the host broke
NULL
nested: stopped, the host broke
NULL
NULL
array(2) {
  [0]=>
  bool(false)
  [1]=>
  string(25) "handled: ordinary:1: mine"
}
array(2) {
  [0]=>
  bool(false)
  [1]=>
  string(23) "error in error handling"
}
