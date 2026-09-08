--TEST--
A forged value reaching __tostring after its class is reclaimed refuses cleanly
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Capabilities;
use DevelopGravity\LuaExt\LuaMethod;
use DevelopGravity\LuaExt\Sandbox;
use DevelopGravity\LuaExt\SandboxConfig;

final class Notice
{
	#[LuaMethod]
	public function id(): int { return 1; }

	#[LuaMethod]
	public function __toString(): string { return 'notice'; }
}

// debugMutate lets a script stamp a genuine proxy metatable onto its own
// table. When the class is then unregistered with no live proxies left, its
// record is reclaimed — and the forged value can still reach the metatable's
// __tostring closure. The refusal must come from the closure's own name
// upvalue, never from the reclaimed record.
$mutate = new Sandbox(new SandboxConfig(
	capabilities: new Capabilities(debugMutate: true),
));
$mutate->registerClass(Notice::class);
$mutate->setGlobal('s', new Notice());

var_dump($mutate->eval('return tostring(s)')[0]);

(void) $mutate->eval('forged = debug.setmetatable({}, debug.getmetatable(s))');
(void) $mutate->eval('s = nil; collectgarbage("collect")');
$mutate->unregister('Notice');

var_dump($mutate->eval('local ok, err = pcall(tostring, forged) return ok, tostring(err)'));

$mutate->close();

?>
--EXPECT--
string(6) "notice"
array(2) {
  [0]=>
  bool(false)
  [1]=>
  string(54) "tostring() received a value that is not a Notice proxy"
}
