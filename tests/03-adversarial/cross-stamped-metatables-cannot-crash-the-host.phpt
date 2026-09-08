--TEST--
Cross-stamping registry metatables under debugMutate cannot crash or starve the host
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);

require __DIR__ . '/../06-vfs/memory-filesystem.inc';

use DevelopGravity\LuaExt\Capabilities;
use DevelopGravity\LuaExt\Exception\RuntimeError;
use DevelopGravity\LuaExt\LuaMethod;
use DevelopGravity\LuaExt\Sandbox;
use DevelopGravity\LuaExt\SandboxConfig;

// A userdata is finalised through whatever metatable it wears at COLLECTION
// time, and debugMutate hands a script debug.getregistry() plus a setmetatable
// that ignores __metatable. So every finalising metatable this extension
// interns is stampable onto every userdata a script can hold -- and each __gc
// must prove the payload is its own before touching a byte, or it reads a
// foreign struct: the file finaliser used to release an error value's bytes
// as if they were a path and a buffer.
final class Remembered extends RuntimeError
{
	public static bool $destroyed = false;

	public function __destruct()
	{
		self::$destroyed = true;
	}
}

final class Held
{
	public static bool $destroyed = false;

	#[LuaMethod]
	public function id(): int { return 1; }

	public function __destruct()
	{
		self::$destroyed = true;
	}
}

$filesystem = new MemoryFileSystem();
$filesystem->files['/f.txt'] = 'contents';

$sandbox = new Sandbox(new SandboxConfig(
	capabilities: new Capabilities(debugMutate: true, vfs: true),
	filesystem: $filesystem,
));
$sandbox->registerClass(Held::class);
$sandbox->setGlobal('obj', new Held());
$sandbox->registerLibrary('host', [
	'fail' => static function (): never { throw new Remembered('held by the error value'); },
]);

// Every finalising metatable in the registry, stamped onto a fresh catchable
// error value and a fresh open file handle, then a full collection. Pre-fix
// this was a wild zend_string_release() inside the collector.
$counts = $sandbox->eval(<<<'LUA'
	-- Warm up the lazily-created metatables so the enumeration sees them all.
	io.open("/f.txt", "r"):close()

	local mts = {}
	for _, value in pairs(debug.getregistry()) do
		if type(value) == "table" and rawget(value, "__gc") ~= nil then
			mts[#mts + 1] = value
		end
	end

	local stamped = 0
	for _, mt in ipairs(mts) do
		local _, err = pcall(host.fail)
		if type(err) == "userdata" then
			debug.setmetatable(err, mt)
			stamped = stamped + 1
		end

		local file = io.open("/f.txt", "r")
		if file ~= nil then
			debug.setmetatable(file, mt)
			stamped = stamped + 1
		end
	end

	collectgarbage("collect")
	collectgarbage("collect")

	return #mts, stamped
LUA, '=cross-stamp');

// At least the error, file and anchored-string metatables were found, and
// every stamp landed on both target kinds.
var_dump($counts[0] >= 3);
var_dump($counts[1] === $counts[0] * 2);

// Detaching an error value's metatable settles its payload at the moment it
// stops being ours: the retained host exception came back across the boundary
// drain when eval() returned, not never.
var_dump(Remembered::$destroyed);

// A setmetatable call that upstream refuses must leave the payload untouched:
// the proxy is still live and still dispatches after the failed call.
$results = $sandbox->eval(
	'local ok = pcall(debug.setmetatable, obj, 42) return ok, obj:id()',
	'=refused-stamp');
var_dump($results[0]);
var_dump($results[1]);
var_dump($sandbox->stats()->liveObjectProxies);

$sandbox->close();

// close() still hands every wrapped object back.
var_dump(Held::$destroyed);

echo "survived\n";

?>
--EXPECT--
bool(true)
bool(true)
bool(true)
bool(false)
int(1)
int(1)
bool(true)
survived
