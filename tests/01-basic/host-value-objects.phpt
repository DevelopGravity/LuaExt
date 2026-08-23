--TEST--
FileStat, ModuleSource and the LuaMethod attribute carry their arguments intact
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\FileStat;
use DevelopGravity\LuaExt\LuaMethod;
use DevelopGravity\LuaExt\ModuleSource;

$stat = new FileStat(1024, 1700000000);
var_dump($stat->size, $stat->mtime, $stat->isDirectory);

$directory = new FileStat(size: 0, mtime: 1, isDirectory: true);
var_dump($directory->isDirectory);

$module = new ModuleSource("return 1\n", '@/lib/one.lua');
var_dump($module->code, $module->chunkName, $module->isBytecode);
var_dump((new ModuleSource('', '=(binary)', true))->isBytecode);

// The attribute is instantiable, and defaults to "the PHP method name" by
// carrying no name of its own.
var_dump((new LuaMethod())->name, (new LuaMethod('greet'))->name);

final class Greeter
{
	#[LuaMethod]
	public function hello(): string
	{
		return 'hello';
	}

	#[LuaMethod('bonjour')]
	public function french(): string
	{
		return 'bonjour';
	}

	// No attribute: invisible to scripts unless an allowlist names it, so
	// adding a public method here can never silently widen the surface.
	public function secret(): string
	{
		return 'secret';
	}
}

foreach ((new ReflectionClass(Greeter::class))->getMethods() as $method) {
	$attributes = $method->getAttributes(LuaMethod::class);

	printf("%-8s %s\n", $method->getName(), $attributes === []
		? 'not exposed'
		: var_export($attributes[0]->newInstance()->name, true));
}

?>
--EXPECT--
int(1024)
int(1700000000)
bool(false)
bool(true)
string(9) "return 1
"
string(13) "@/lib/one.lua"
bool(false)
bool(true)
NULL
string(5) "greet"
hello    NULL
french   'bonjour'
secret   not exposed
