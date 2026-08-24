--TEST--
A table holding both t[1] and t["1"] is refused rather than silently merged
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Exception\ConversionError;
use DevelopGravity\LuaExt\Sandbox;

$sandbox = new Sandbox();

/**
 * Lua table iteration order is not defined, so which of the two colliding keys
 * is reported is not either. What is fixed is that the conversion is refused
 * and that the message names a key.
 */
function refuse(Sandbox $sandbox, string $chunk): void
{
	try {
		$converted = $sandbox->eval($chunk);
		printf("NOT REFUSED: %s\n", var_export($converted, true));
	} catch (ConversionError $error) {
		printf("%s collides=%s\n",
			$error::class,
			var_export(str_contains($error->getMessage(), 'collides'), true));
	}
}

// PHP folds the string key "1" onto the integer key 1, so a table carrying
// both describes two values PHP can only hold as one. Choosing a winner would
// be data loss with no diagnostic.
refuse($sandbox, 'return { [1] = "integer", ["1"] = "string" }');
refuse($sandbox, 'return { [-7] = "integer", ["-7"] = "string" }');
refuse($sandbox, 'return { [0] = "integer", ["0"] = "string" }');
refuse($sandbox, 'return { { [1] = "integer", ["1"] = "string" } }');

// Keys PHP does not fold are not collisions and must still convert: "01" and
// " 1" and "1.0" are string keys to PHP, distinct from the integer 1.
[$fine] = $sandbox->eval('return { [1] = "integer", ["01"] = "a", [" 1"] = "b", ["1.0"] = "c" }');
var_dump(count($fine), $fine[1], $fine['01'], $fine[' 1'], $fine['1.0']);

// The sandbox is still usable after a refusal: the failure is a conversion
// result, not a broken interpreter.
var_dump($sandbox->eval('return "still here"'));

?>
--EXPECT--
DevelopGravity\LuaExt\Exception\ConversionError collides=true
DevelopGravity\LuaExt\Exception\ConversionError collides=true
DevelopGravity\LuaExt\Exception\ConversionError collides=true
DevelopGravity\LuaExt\Exception\ConversionError collides=true
int(4)
string(7) "integer"
string(1) "a"
string(1) "b"
string(1) "c"
array(1) {
  [0]=>
  string(10) "still here"
}
