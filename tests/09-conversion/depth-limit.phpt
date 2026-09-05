--TEST--
Conversion depth is capped in both directions
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Exception\ConversionError;
use DevelopGravity\LuaExt\Limits;
use DevelopGravity\LuaExt\Sandbox;
use DevelopGravity\LuaExt\SandboxConfig;

/** Build an array nested exactly $levels containers deep. */
function nest(int $levels): array
{
	$node = ['leaf' => true];

	for ($level = 1; $level < $levels; $level++) {
		$node = ['child' => $node];
	}

	return $node;
}

const LUA_NEST = <<<'LUA'
	local function nest(levels)
		local node = { leaf = true }
		for _ = 2, levels do node = { child = node } end
		return node
	end
	return nest(%d)
	LUA;

$sandbox = new Sandbox();

// Limits::$maxConversionDepth defaults to 64, counted in containers. Both
// directions recurse on the C stack, so the cap is what stops a nested array
// from turning a conversion into a stack overflow.
$sandbox->setGlobal('deep', nest(64));
var_dump($sandbox->eval('return type(deep.child)'));

try {
	$sandbox->setGlobal('deeper', nest(65));
	echo "NOT REFUSED php->lua\n";
} catch (ConversionError $error) {
	printf("php->lua %s depth=%s\n",
		$error::class,
		var_export(str_contains($error->getMessage(), 'deeper than'), true));
}

[$converted] = $sandbox->eval(sprintf(LUA_NEST, 64));
$descents = 0;

while (isset($converted['child'])) {
	$converted = $converted['child'];
	$descents++;
}

var_dump($descents, $converted['leaf']);

try {
	printf("NOT REFUSED lua->php: %s\n",
		var_export($sandbox->eval(sprintf(LUA_NEST, 65)), true));
} catch (ConversionError $error) {
	printf("lua->php %s depth=%s\n",
		$error::class,
		var_export(str_contains($error->getMessage(), 'deeper than'), true));
}

// A depth refusal is not a broken sandbox, and the global that failed to be
// written was never partially written either.
var_dump($sandbox->eval('return deeper == nil, type(deep)'));

// A HOST CANNOT CONFIGURE ITS WAY TO A C STACK OVERFLOW.
//
// Both conversion directions recurse on the C stack, so maxConversionDepth is
// clamped to an internal ceiling however large a host sets it. Without that
// clamp, `maxConversionDepth: 100000` would be an ordinary-looking setting that
// hands a script a segfault, and no Lua-level limit counts C frames.
//
// The table below is built with a LOOP rather than a literal on purpose: a
// deeply nested literal is refused by the parser's own C-stack guard long
// before the converter sees it, which would test the wrong thing entirely.
$build = 'local root = {} local c = root for i = 1, 5000 do c.n = {} c = c.n end return root';

foreach ([64, 100000] as $configured) {
	$wide = new Sandbox(new SandboxConfig(
		limits: new Limits(memoryBytes: 268435456, maxConversionDepth: $configured),
	));

	try {
		(void) $wide->eval($build, '=deep');
		printf("configured %-6d NOT REFUSED\n", $configured);
	} catch (ConversionError $error) {
		// The message names the depth actually enforced, which is the clamp made
		// visible: ask for 100000 and it says 512.
		preg_match('/deeper than (\d+) levels/', $error->getMessage(), $matches);
		printf("configured %-6d enforced %s\n", $configured, $matches[1] ?? '?');
	}

	$wide->close();
}

?>
--EXPECT--
array(1) {
  [0]=>
  string(5) "table"
}
php->lua DevelopGravity\LuaExt\Exception\ConversionError depth=true
int(63)
bool(true)
lua->php DevelopGravity\LuaExt\Exception\ConversionError depth=true
array(2) {
  [0]=>
  bool(true)
  [1]=>
  string(5) "table"
}
configured 64     enforced 64
configured 100000 enforced 512
