--TEST--
Proxies cannot be forged, their metatables cannot be read, and debug grants stay contained
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Capabilities;
use DevelopGravity\LuaExt\Exception\ConversionError;
use DevelopGravity\LuaExt\LuaMethod;
use DevelopGravity\LuaExt\Sandbox;
use DevelopGravity\LuaExt\SandboxConfig;

final class Secret
{
	#[LuaMethod]
	public function id(): int { return 1; }
}

$sandbox = new Sandbox();
$sandbox->registerClass(Secret::class);
$sandbox->setGlobal('s', new Secret());

// The metatable is locked; raw probing learns nothing scripts can use.
var_dump($sandbox->eval('return getmetatable(s)')[0]);
var_dump($sandbox->eval('return rawequal(s, s)')[0]);
$sandbox->close();

// debugIntrospect gains nothing over the proxy's insides.
$introspect = new Sandbox(new SandboxConfig(
	capabilities: new Capabilities(debugIntrospect: true),
));
$introspect->registerClass(Secret::class);
$introspect->setGlobal('s', new Secret());
var_dump($introspect->eval('return debug.getinfo(function() end) ~= nil, getmetatable(s)'));
$introspect->close();

// Under debugMutate -- documented as escaping most guarantees -- the narrower
// assertions still hold: no crash, no forged unwrap, no reach to the wrapped
// object.
$mutate = new Sandbox(new SandboxConfig(
	capabilities: new Capabilities(debugMutate: true),
));
$mutate->registerClass(Secret::class);
$mutate->setGlobal('s', new Secret());

// A TABLE wearing a genuine proxy metatable reaches the metamethods and the
// dispatch closures as an arbitrary value; every gate must answer, none crash.
// Probed BEFORE stripping s, while debug.getmetatable(s) still yields the real
// metatable.
var_dump($mutate->eval(<<<'LUA'
	local mt = debug.getmetatable(s)
	local forged = debug.setmetatable({}, mt)
	local forged2 = debug.setmetatable({}, mt)
	-- Two DISTINCT tables: identical values are primitively equal and never
	-- reach __eq at all.
	local eqOk, eqResult = pcall(function() return forged == forged2 end)
	local callOk = pcall(function() return forged:id() end)
	return eqOk, eqResult == true, callOk
LUA));

// A stripped proxy no longer unwraps: refusal, never a stranger's object.
(void) $mutate->eval('debug.setmetatable(s, {})');
try {
	$mutate->getGlobal('s');
	echo "unwrapped anyway\n";
} catch (ConversionError) {
	echo "stripped proxy refused\n";
}
$mutate->close();

?>
--EXPECT--
bool(false)
bool(true)
array(2) {
  [0]=>
  bool(true)
  [1]=>
  bool(false)
}
array(3) {
  [0]=>
  bool(true)
  [1]=>
  bool(false)
  [2]=>
  bool(false)
}
stripped proxy refused
