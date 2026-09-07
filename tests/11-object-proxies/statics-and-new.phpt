--TEST--
Marked statics and a marked constructor publish on the class table
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\LuaMethod;
use DevelopGravity\LuaExt\Sandbox;

final class Money
{
	#[LuaMethod]
	public function __construct(private int $cents = 0) {}

	#[LuaMethod]
	public static function zero(): self
	{
		return new self(0);
	}

	#[LuaMethod('parse')]
	public static function fromString(string $text): self
	{
		return new self((int) round((float) $text * 100));
	}

	#[LuaMethod]
	public static function flip(string $text): string
	{
		return strrev($text);
	}

	#[LuaMethod]
	public function cents(): int
	{
		return $this->cents;
	}
}

$sandbox = new Sandbox();
$sandbox->registerClass(Money::class, luaName: 'money');

// Statics are plain functions; a returned instance is a working proxy.
var_dump($sandbox->eval('return money.zero():cents()')[0]);
var_dump($sandbox->eval('return money.parse("12.34"):cents()')[0]);

// A pure helper static touches no proxy machinery at all.
var_dump($sandbox->eval('return money.flip("abc")')[0]);

// The marked constructor is .new, with and without arguments.
var_dump($sandbox->eval('return money.new(250):cents()')[0]);
var_dump($sandbox->eval('return money.new():cents()')[0]);

// Colon on a static passes the class table: catchable, message names the fix.
var_dump($sandbox->eval('local ok, err = pcall(function() return money:zero() end) return ok, tostring(err)'));

// A class with only instance methods plants no global.
final class Quiet
{
	#[LuaMethod]
	public function ping(): int { return 1; }
}
$sandbox->registerClass(Quiet::class);
var_dump($sandbox->eval('return Quiet == nil')[0]);

$sandbox->close();

?>
--EXPECTF--
int(0)
int(1234)
string(3) "cba"
int(250)
int(0)
array(2) {
  [0]=>
  bool(false)
  [1]=>
  string(%d) "static 'zero' is called with a dot (money.zero(...))"
}
bool(true)
