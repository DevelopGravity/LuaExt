--TEST--
LuaMethod is registered as an internal attribute that only targets methods
--EXTENSIONS--
luaext
--FILE--
<?php

use DevelopGravity\LuaExt\LuaMethod;

// The stub carries no #[Attribute] marker; MINIT registers it instead, so this
// checks the registration rather than anything gen_stub produced.
$markers = (new ReflectionClass(LuaMethod::class))->getAttributes(Attribute::class);

var_dump(count($markers));
var_dump($markers[0]->newInstance()->flags === Attribute::TARGET_METHOD);

class Host
{
	#[LuaMethod]
	public function plain(): void {}

	#[LuaMethod('greet')]
	public function hello(): void {}
}

foreach (['plain', 'hello'] as $method) {
	$attributes = (new ReflectionMethod(Host::class, $method))->getAttributes(LuaMethod::class);

	var_dump(count($attributes));
	var_dump($attributes[0]->getArguments());
}

?>
--EXPECT--
int(1)
bool(true)
int(1)
array(0) {
}
int(1)
array(1) {
  [0]=>
  string(5) "greet"
}
