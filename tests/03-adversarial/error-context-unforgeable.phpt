--TEST--
The Lua context on an exception is not reachable by name from PHP
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

use DevelopGravity\LuaExt\Exception\CapabilityError;
use DevelopGravity\LuaExt\Exception\CpuLimitError;
use DevelopGravity\LuaExt\Exception\LuaThrowable;
use DevelopGravity\LuaExt\Exception\RuntimeError;

// getLuaTrace()/getSandbox()/getChunkName()/getLuaLine() say where inside a
// sandbox a failure happened, and a host reads them to decide whether a script
// or its own code was at fault. They are therefore stored under a property name
// mangled as private to a class that does not exist, which PHP has no syntax to
// address: writing the obvious names creates unrelated properties and changes
// nothing, and the real slot never appears in the property table of an
// exception the sandbox did not throw.

function report(string $label, LuaThrowable $error): void
{
	printf("%-16s trace=%s string=%s sandbox=%s chunk=%s line=%s\n",
		$label,
		var_export($error->getLuaTrace(), true),
		var_export($error->getLuaTraceAsString(), true),
		var_export($error->getSandbox(), true),
		var_export($error->getChunkName(), true),
		var_export($error->getLuaLine(), true));
}

/** @return list<string> every property-table key naming the extension */
function internalKeys(object $error): array
{
	return array_values(array_filter(
		array_keys((array) $error),
		static fn (string $key): bool => str_contains($key, 'luaext')));
}

// An exception the host built itself never originated in Lua, and says so.
$error = new RuntimeError('the host made this');
report('constructed', $error);

// LuaException and LuaLogicException carry their own compiled copy of these
// five methods. They must not drift: a limit breach and a configuration mistake
// have to report their Lua context identically or neither answer means
// anything.
report('other root', new CapabilityError('also the host'));
report('fatal branch', new CpuLimitError('still the host'));

// Writing the obvious names does not reach the storage.
@$error->luaTrace = [['source' => 'trusted.lua', 'what' => 'Lua', 'currentLine' => 1,
	'name' => null, 'nameWhat' => '', 'lineDefined' => 1]];
@$error->sandbox = 'anything';
@$error->chunkName = 'trusted.lua';
@$error->luaLine = 1;

report('after forging', $error);

// The forged properties exist -- as ordinary dynamic properties that nothing
// in the extension reads. The slot the accessors actually use is not there.
var_dump(array_slice(array_keys((array) $error), 7));
var_dump(internalKeys($error));
var_dump(internalKeys(new RuntimeError('untouched')));

?>
--EXPECT--
constructed      trace=NULL string='' sandbox=NULL chunk=NULL line=NULL
other root       trace=NULL string='' sandbox=NULL chunk=NULL line=NULL
fatal branch     trace=NULL string='' sandbox=NULL chunk=NULL line=NULL
after forging    trace=NULL string='' sandbox=NULL chunk=NULL line=NULL
array(4) {
  [0]=>
  string(8) "luaTrace"
  [1]=>
  string(7) "sandbox"
  [2]=>
  string(9) "chunkName"
  [3]=>
  string(7) "luaLine"
}
array(0) {
}
array(0) {
}
