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
// or its own code was at fault. They are stored in DECLARED private
// properties of the two base classes -- which no other scope can address by
// name: writing the obvious names from outside creates unrelated dynamic
// properties and changes nothing. Declared, not hand-mangled dynamic keys,
// because the engine's access control short-circuits to success for a
// dynamic property whose name starts with NUL -- the old scheme let
// get_object_vars() from ANY scope read the trace and the live sandbox out
// of a thrown exception.

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
// in the extension reads. The declared slots sit beside them under their
// class-mangled keys, which no assignment above could reach.
var_dump(array_map(
	// The declared slots' keys carry real NUL bytes; made printable so the
	// expectation block can spell them.
	static fn (string $key): string => str_replace("\0", '#', $key),
	array_slice(array_keys((array) $error), 7),
));
var_dump(internalKeys($error));
var_dump(internalKeys(new RuntimeError('untouched')));

// The other direction, and the reason the storage is DECLARED: an exception
// the sandbox threw carries a real trace and a live sandbox, and neither may
// leak through get_object_vars() from a foreign scope. (The base Exception
// fields are protected or private too, so from here the answer is empty.)
$sandbox = new DevelopGravity\LuaExt\Sandbox();

try {
	(void) $sandbox->eval('error("boom")', '=leaky');
	echo "NOT THROWN\n";
} catch (RuntimeError $thrown) {
	var_dump($thrown->getSandbox() === $sandbox, $thrown->getChunkName());
	var_dump(get_object_vars($thrown));
}

$sandbox->close();

?>
--EXPECT--
constructed      trace=NULL string='' sandbox=NULL chunk=NULL line=NULL
other root       trace=NULL string='' sandbox=NULL chunk=NULL line=NULL
fatal branch     trace=NULL string='' sandbox=NULL chunk=NULL line=NULL
after forging    trace=NULL string='' sandbox=NULL chunk=NULL line=NULL
array(6) {
  [0]=>
  string(54) "#DevelopGravity\LuaExt\Exception\LuaException#luaTrace"
  [1]=>
  string(56) "#DevelopGravity\LuaExt\Exception\LuaException#luaSandbox"
  [2]=>
  string(8) "luaTrace"
  [3]=>
  string(7) "sandbox"
  [4]=>
  string(9) "chunkName"
  [5]=>
  string(7) "luaLine"
}
array(0) {
}
array(0) {
}
bool(true)
string(5) "leaky"
array(0) {
}
