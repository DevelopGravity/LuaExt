--TEST--
load() meters a reader that feeds the parser numbers, not only strings
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Capabilities;
use DevelopGravity\LuaExt\Limits;
use DevelopGravity\LuaExt\Sandbox;
use DevelopGravity\LuaExt\SandboxConfig;

// Lua's reader protocol accepts anything lua_isstring() accepts, and that
// includes numbers -- they are coerced and parsed like any other source. The
// wrapper that enforces Limits::$maxSourceBytes used to test for a string
// return and wave everything else through, so a reader returning numbers fed
// the parser unmetered: with no interrupt check inside the lexer's loop, an
// endless one parsed forever regardless of every configured limit.

$sandbox = new Sandbox(new SandboxConfig(
	capabilities: (new Capabilities())->with(compileAtRuntime: true),
	limits: new Limits(maxSourceBytes: 4096),
));

// An endless stream of numeric "source". Unmetered this never returns; metered
// it dies on the byte ceiling within a few hundred reader calls, surfacing as
// load()'s ordinary fail-plus-message return the way any mid-parse refusal
// does.
[$failed, $message] = $sandbox->eval(<<<'LUA'
	local chunk, err = load(function()
		return 12345678
	end)

	return chunk == nil, tostring(err)
	LUA, '=numbers-forever');

printf("endless numbers => %s\n",
	$failed && str_contains((string) $message, '4096 byte source limit')
		? 'refused at the ceiling'
		: sprintf('UNEXPECTED (%s)', var_export($message, true)));

// A reader mixing numbers into otherwise valid source is billed for their
// string forms like anything else, and still compiles under the ceiling.
[$value] = $sandbox->eval(<<<'LUA'
	local pieces = {"return ", 4, "2"}
	local index = 0

	local chunk = load(function()
		index = index + 1
		return pieces[index]
	end)

	return chunk()
	LUA, '=numbers-as-source');

printf("mixed reader    => %d\n", $value);

$sandbox->close();

?>
--EXPECT--
endless numbers => refused at the ceiling
mixed reader    => 42
