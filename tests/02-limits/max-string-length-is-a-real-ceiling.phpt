--TEST--
Limits::$maxStringLength refuses every way a script can materialise an oversized string
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Exception\ConversionError;
use DevelopGravity\LuaExt\Limits;
use DevelopGravity\LuaExt\Sandbox;
use DevelopGravity\LuaExt\SandboxConfig;

// The limit is enforced at the interpreter's single choke point (the patched
// luaS_createlngstrobj), so it does not matter which door the string tries:
// rep, concat, format, table.concat and load results all pass through it.
// Short strings are interned through a different path, so the effective
// floor of the limit is Lua's 40-byte internment size.
$sandbox = new Sandbox(new SandboxConfig(
	limits: new Limits(maxStringLength: 1024),
));

[$report] = $sandbox->eval('
	local outcomes = {}

	local function attempt(label, build)
		local ok, err = pcall(build)
		outcomes[#outcomes + 1] = string.format("%s: %s", label,
			ok and "ALLOWED" or tostring(err))
	end

	attempt("rep", function () return string.rep("x", 2048) end)
	attempt("concat", function ()
		local half = string.rep("y", 1000)
		return half .. half
	end)
	attempt("format", function ()
		local half = string.rep("z", 600)
		return string.format("%s%s", half, half)
	end)
	attempt("table.concat", function ()
		local parts = {}
		for index = 1, 64 do parts[index] = string.rep("w", 32) end
		return table.concat(parts)
	end)

	-- At the limit exactly is allowed: the ceiling is inclusive.
	attempt("at the limit", function () return string.rep("k", 1024) end)

	return table.concat(outcomes, "\n")
', '=gate');

echo $report, "\n";

// The refusal is catchable, so the sandbox keeps working afterwards.
[$still] = $sandbox->eval('return string.rep("a", 8)', '=after');
var_dump($still);

// The PHP side of the same ceiling: an oversized host string is refused at
// conversion, before the interpreter ever sees it, with the path named.
try {
	$sandbox->setGlobal('huge', str_repeat('h', 2048));
	echo "conversion: NOT REFUSED\n";
} catch (ConversionError $error) {
	printf("conversion: %s\n", $error->getMessage());
}

$sandbox->close();

// Zero stays "no limit".
$unlimited = new Sandbox(new SandboxConfig(limits: new Limits(maxStringLength: 0)));
[$length] = $unlimited->eval('return #string.rep("x", 1 << 20)', '=unlimited');
var_dump($length);
$unlimited->close();

?>
--EXPECT--
rep: string exceeds Limits::$maxStringLength
concat: string exceeds Limits::$maxStringLength
format: string exceeds Limits::$maxStringLength
table.concat: string exceeds Limits::$maxStringLength
at the limit: ALLOWED
string(8) "aaaaaaaa"
conversion: Cannot convert a PHP string of 2048 bytes to Lua: the sandbox's Limits::$maxStringLength is 1024 at value
int(1048576)
