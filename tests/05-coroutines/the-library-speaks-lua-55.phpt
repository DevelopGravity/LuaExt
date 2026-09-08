--TEST--
The coroutine library carries Lua 5.5's shapes: optional close, isnone, thread wording
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Sandbox;

// The replacement library must speak the language it replaces. 5.5 made
// coroutine.close's argument optional (defaulting to the running coroutine,
// the self-close), treats an explicit nil as an argument everywhere, and
// names the type "thread" in its argument errors -- each of these diverged
// here once, from 5.4-era idioms.
$sandbox = new Sandbox();

$probe = static function (string $label, string $expression) use ($sandbox): void {
	$result = $sandbox->eval(
		"local ok, a, b = pcall(function() return {$expression} end) " .
		'if ok then return "ok", tostring(a) .. "/" .. tostring(b) end return "err", tostring(a)',
		'=probe',
	);

	printf("%-18s %s: %s\n", $label, $result[0], $result[1]);
};

// close() with no argument closes the running coroutine: the self-close does
// not return, and the resume that started it sees a clean end.
$probe('close-optional',
	'(function() local co = coroutine.create(function() coroutine.close() end) ' .
	'return coroutine.resume(co) end)()');

$probe('close-suspended', 'coroutine.close(coroutine.create(function() end))');

// An explicit nil is an argument, and an argument that is not a thread is an
// error -- for close and isyieldable both.
$probe('close-nil', 'coroutine.close(nil)');
$probe('isyieldable-nil', 'coroutine.isyieldable(nil)');
$probe('isyieldable-self',
	'select(2, coroutine.resume(coroutine.create(function() return coroutine.isyieldable() end)))');

// The argument errors say "thread", as upstream's getco does.
$probe('resume-wording', 'coroutine.resume(42)');
$probe('status-wording', 'coroutine.status("x")');

$sandbox->close();

?>
--EXPECT--
close-optional     ok: true/nil
close-suspended    ok: true/nil
close-nil          err: probe:1: bad argument #1 to 'close' (thread expected, got nil)
isyieldable-nil    err: probe:1: bad argument #1 to 'isyieldable' (thread expected, got nil)
isyieldable-self   ok: true/nil
resume-wording     err: probe:1: bad argument #1 to 'resume' (thread expected, got number)
status-wording     err: probe:1: bad argument #1 to 'status' (thread expected, got string)
