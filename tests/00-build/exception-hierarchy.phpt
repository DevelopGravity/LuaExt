--TEST--
The exception hierarchy has the documented parents and abstractness
--EXTENSIONS--
luaext
--FILE--
<?php

declare(strict_types=1);
const NS = 'DevelopGravity\\LuaExt\\Exception\\';

// A script may catch a RuntimeError with pcall but never a FatalError, and the
// host-misuse classes never cross into Lua at all -- which is exactly what
// these three roots separate.
$parents = [
	'LuaException' => 'RuntimeException',
	'LuaLogicException' => 'LogicException',
	'RuntimeError' => NS . 'LuaException',
	'VfsError' => NS . 'RuntimeError',
	'ModuleNotFoundError' => NS . 'RuntimeError',
	'FatalError' => NS . 'LuaException',
	'SyntaxError' => NS . 'FatalError',
	'MemoryLimitError' => NS . 'FatalError',
	'CpuLimitError' => NS . 'FatalError',
	'WallClockLimitError' => NS . 'FatalError',
	'OutputLimitError' => NS . 'FatalError',
	'CoroutineLimitError' => NS . 'FatalError',
	'HostAbortError' => NS . 'FatalError',
	'ErrorHandlerError' => NS . 'FatalError',
	'PanicError' => NS . 'FatalError',
	'ConversionError' => NS . 'FatalError',
	'ConfigurationError' => NS . 'LuaLogicException',
	'CapabilityError' => NS . 'LuaLogicException',
	'ClosedSandboxError' => NS . 'LuaLogicException',
	'ThreadAffinityError' => NS . 'LuaLogicException',
];

var_dump((new ReflectionClass(NS . 'LuaThrowable'))->isInterface());
var_dump(is_a(NS . 'LuaThrowable', 'Throwable', true));

foreach ($parents as $name => $parent) {
	$reflection = new ReflectionClass(NS . $name);

	printf("%-20s parent=%s throwable=%s abstract=%s\n",
		$name,
		var_export(get_parent_class(NS . $name) === $parent, true),
		var_export($reflection->implementsInterface(NS . 'LuaThrowable'), true),
		var_export($reflection->isAbstract(), true));
}

?>
--EXPECT--
bool(true)
bool(true)
LuaException         parent=true throwable=true abstract=true
LuaLogicException    parent=true throwable=true abstract=true
RuntimeError         parent=true throwable=true abstract=false
VfsError             parent=true throwable=true abstract=false
ModuleNotFoundError  parent=true throwable=true abstract=false
FatalError           parent=true throwable=true abstract=true
SyntaxError          parent=true throwable=true abstract=false
MemoryLimitError     parent=true throwable=true abstract=false
CpuLimitError        parent=true throwable=true abstract=false
WallClockLimitError  parent=true throwable=true abstract=false
OutputLimitError     parent=true throwable=true abstract=false
CoroutineLimitError  parent=true throwable=true abstract=false
HostAbortError       parent=true throwable=true abstract=false
ErrorHandlerError    parent=true throwable=true abstract=false
PanicError           parent=true throwable=true abstract=false
ConversionError      parent=true throwable=true abstract=false
ConfigurationError   parent=true throwable=true abstract=false
CapabilityError      parent=true throwable=true abstract=false
ClosedSandboxError   parent=true throwable=true abstract=false
ThreadAffinityError  parent=true throwable=true abstract=false
