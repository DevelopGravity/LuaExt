--TEST--
Objects, resources and references to them have no Lua representation
--EXTENSIONS--
luaext
--XFAIL--
Needs Sandbox::eval()/setGlobal(), which land with the execution subsystem.
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Exception\ConversionError;
use DevelopGravity\LuaExt\Sandbox;

enum Suit: string
{
	case Hearts = 'H';
}

$sandbox = new Sandbox();
$handle = fopen('php://memory', 'rb');
$object = new stdClass();

// Object identity never crosses the boundary. registerObject() is the one
// deliberate bridge, and it exposes bound methods rather than the object, so
// there is nothing here to guess at: refusing is the design, not a gap.
$rejected = [
	'stdClass' => $object,
	'closure' => static fn(): int => 1,
	'enum case' => Suit::Hearts,
	'resource' => $handle,
	'array holding an object' => ['inner' => ['leaf' => $object]],
	'array holding a resource' => [$handle],
	'array holding a reference to an object' => ['ref' => &$object],
	'array holding a reference to a resource' => ['ref' => &$handle],
];

foreach ($rejected as $label => $value) {
	try {
		$sandbox->setGlobal('probe', $value);
		printf("%s: NOT REFUSED\n", $label);
	} catch (ConversionError $error) {
		printf("%s: %s\n", $label, $error::class);
	}
}

// A reference is transparent, so a reference to something Lua *can* hold is
// converted rather than refused: it is the referent that decides.
$number = 42;
$sandbox->setGlobal('fine', ['ref' => &$number]);
var_dump($sandbox->eval('return fine.ref'));

// The path in the message points at the element responsible, not merely at the
// argument, so a refusal inside a large payload is actionable.
try {
	$sandbox->setGlobal('probe', ['inner' => ['leaf' => $object]]);
} catch (ConversionError $error) {
	var_dump(str_contains($error->getMessage(), 'value["inner"]["leaf"]'));
}

// Nothing was written on the way to any of the refusals.
var_dump($sandbox->eval('return probe == nil'));

fclose($handle);

?>
--EXPECT--
stdClass: DevelopGravity\LuaExt\Exception\ConversionError
closure: DevelopGravity\LuaExt\Exception\ConversionError
enum case: DevelopGravity\LuaExt\Exception\ConversionError
resource: DevelopGravity\LuaExt\Exception\ConversionError
array holding an object: DevelopGravity\LuaExt\Exception\ConversionError
array holding a resource: DevelopGravity\LuaExt\Exception\ConversionError
array holding a reference to an object: DevelopGravity\LuaExt\Exception\ConversionError
array holding a reference to a resource: DevelopGravity\LuaExt\Exception\ConversionError
array(1) {
  [0]=>
  int(42)
}
bool(true)
array(1) {
  [0]=>
  bool(true)
}
