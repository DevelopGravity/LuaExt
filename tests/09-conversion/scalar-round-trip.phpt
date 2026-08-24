--TEST--
Every scalar with a Lua counterpart survives a round trip unchanged
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Sandbox;

$sandbox = new Sandbox();

// Identity, not merely equality: a round trip that turns 0 into false or 1.0
// into 1 has lost information even though == would still agree.
$values = [
	'null' => null,
	'false' => false,
	'true' => true,
	'int zero' => 0,
	'int positive' => 12345,
	'int negative' => -12345,
	'float' => 1.5,
	'float integral' => 2.0,
	'float negative zero' => -0.0,
	'string empty' => '',
	'string' => 'hello',
	'string numeric' => '42',
];

foreach ($values as $label => $value) {
	$sandbox->setGlobal('probe', $value);
	$returned = $sandbox->getGlobal('probe');

	printf("%-20s %-6s %s\n",
		$label,
		get_debug_type($returned),
		var_export($returned === $value, true));
}

// A float that is not exactly representable must come back bit-identical
// rather than rounded through a decimal rendering.
$sandbox->setGlobal('probe', 0.1 + 0.2);
var_dump($sandbox->getGlobal('probe') === 0.1 + 0.2);

?>
--EXPECT--
null                 null   true
false                bool   true
true                 bool   true
int zero             int    true
int positive         int    true
int negative         int    true
float                float  true
float integral       float  true
float negative zero  float  true
string empty         string true
string               string true
string numeric       string true
bool(true)
