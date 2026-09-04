/* This is a generated file, edit the .stub.php file instead.
 * Stub hash: 0c928f59aa46493fcc284422b51560c28ee5efc7 */

ZEND_BEGIN_ARG_INFO_EX(arginfo_class_DevelopGravity_LuaExt_LuaMethod___construct, 0, 0, 0)
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, name, IS_STRING, 1, "null")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_class_DevelopGravity_LuaExt_Capabilities___construct, 0, 0, 0)
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, loadBytecode, _IS_BOOL, 0, "false")
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, compileAtRuntime, _IS_BOOL, 0, "false")
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, dumpBytecode, _IS_BOOL, 0, "false")
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, require, _IS_BOOL, 0, "false")
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, vfs, _IS_BOOL, 0, "false")
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, vfsWrite, _IS_BOOL, 0, "false")
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, coroutines, _IS_BOOL, 0, "true")
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, osTime, _IS_BOOL, 0, "true")
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, osEnv, _IS_BOOL, 0, "false")
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, osEnvAllowList, IS_ARRAY, 0, "[]")
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, debugTraceback, _IS_BOOL, 0, "true")
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, debugIntrospect, _IS_BOOL, 0, "false")
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, debugMutate, _IS_BOOL, 0, "false")
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, debugHooks, _IS_BOOL, 0, "false")
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, utf8, _IS_BOOL, 0, "true")
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, gcControl, _IS_BOOL, 0, "false")
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, warn, _IS_BOOL, 0, "false")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_class_DevelopGravity_LuaExt_Capabilities_untrusted, 0, 0, DevelopGravity\\LuaExt\\Capabilities, 0)
ZEND_END_ARG_INFO()

#define arginfo_class_DevelopGravity_LuaExt_Capabilities_trusted arginfo_class_DevelopGravity_LuaExt_Capabilities_untrusted

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_class_DevelopGravity_LuaExt_Capabilities_with, 0, 0, DevelopGravity\\LuaExt\\Capabilities, 0)
	ZEND_ARG_VARIADIC_TYPE_INFO(0, overrides, IS_MIXED, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_class_DevelopGravity_LuaExt_Limits___construct, 0, 0, 0)
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, memoryBytes, IS_LONG, 1, "33554432")
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, cpuSeconds, IS_DOUBLE, 1, "1.0")
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, wallClockSeconds, IS_DOUBLE, 1, "5.0")
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, outputBytes, IS_LONG, 0, "1048576")
	ZEND_ARG_OBJ_INFO_WITH_DEFAULT_VALUE(0, outputOverflow, DevelopGravity\\LuaExt\\OverflowBehavior, 0, "DevelopGravity\\LuaExt\\OverflowBehavior::Fail")
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, maxLiveCoroutines, IS_LONG, 0, "64")
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, maxCoroutineDepth, IS_LONG, 0, "16")
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, maxCallDepth, IS_LONG, 0, "200")
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, maxModules, IS_LONG, 0, "64")
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, maxRequireDepth, IS_LONG, 0, "16")
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, maxStringLength, IS_LONG, 0, "67108864")
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, maxSourceBytes, IS_LONG, 0, "1048576")
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, maxConversionDepth, IS_LONG, 0, "64")
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, maxCachedChunks, IS_LONG, 0, "64")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_class_DevelopGravity_LuaExt_Limits_with, 0, 0, DevelopGravity\\LuaExt\\Limits, 0)
	ZEND_ARG_VARIADIC_TYPE_INFO(0, overrides, IS_MIXED, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_class_DevelopGravity_LuaExt_VfsQuota___construct, 0, 0, 0)
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, maxOpenHandles, IS_LONG, 0, "16")
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, maxFileBytes, IS_LONG, 0, "1048576")
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, maxTotalBytes, IS_LONG, 0, "8388608")
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, maxFiles, IS_LONG, 0, "128")
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, maxOperations, IS_LONG, 0, "10000")
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, maxPathLength, IS_LONG, 0, "255")
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, maxPathDepth, IS_LONG, 0, "16")
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, billWallTime, _IS_BOOL, 0, "false")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_class_DevelopGravity_LuaExt_VfsQuota_with, 0, 0, DevelopGravity\\LuaExt\\VfsQuota, 0)
	ZEND_ARG_VARIADIC_TYPE_INFO(0, overrides, IS_MIXED, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_class_DevelopGravity_LuaExt_SandboxConfig___construct, 0, 0, 0)
	ZEND_ARG_OBJ_INFO_WITH_DEFAULT_VALUE(0, capabilities, DevelopGravity\\LuaExt\\Capabilities, 1, "null")
	ZEND_ARG_OBJ_INFO_WITH_DEFAULT_VALUE(0, limits, DevelopGravity\\LuaExt\\Limits, 1, "null")
	ZEND_ARG_OBJ_INFO_WITH_DEFAULT_VALUE(0, filesystem, DevelopGravity\\LuaExt\\FileSystem, 1, "null")
	ZEND_ARG_OBJ_INFO_WITH_DEFAULT_VALUE(0, vfsQuota, DevelopGravity\\LuaExt\\VfsQuota, 1, "null")
	ZEND_ARG_OBJ_INFO_WITH_DEFAULT_VALUE(0, moduleResolver, DevelopGravity\\LuaExt\\ModuleResolver, 1, "null")
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, modulePaths, IS_ARRAY, 0, "[\'/?.lua\', \'/?/init.lua\']")
	ZEND_ARG_OBJ_INFO_WITH_DEFAULT_VALUE(0, outputMode, DevelopGravity\\LuaExt\\OutputMode, 0, "DevelopGravity\\LuaExt\\OutputMode::Buffer")
	ZEND_ARG_OBJ_INFO_WITH_DEFAULT_VALUE(0, outputCallback, Closure, 1, "null")
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, outputChunkBytes, IS_LONG, 0, "8192")
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, seed, IS_LONG, 1, "null")
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, deterministic, _IS_BOOL, 0, "false")
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, cacheCompiledChunks, _IS_BOOL, 0, "false")
	ZEND_ARG_OBJ_INFO_WITH_DEFAULT_VALUE(0, sealMode, DevelopGravity\\LuaExt\\SealMode, 0, "DevelopGravity\\LuaExt\\SealMode::Checksum")
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, bytecodeKey, IS_STRING, 1, "null")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_class_DevelopGravity_LuaExt_SandboxConfig_with, 0, 0, DevelopGravity\\LuaExt\\SandboxConfig, 0)
	ZEND_ARG_VARIADIC_TYPE_INFO(0, overrides, IS_MIXED, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_class_DevelopGravity_LuaExt_SandboxStats___construct, 0, 0, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_DevelopGravity_LuaExt_SandboxStats_jsonSerialize, 0, 0, IS_ARRAY, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_class_DevelopGravity_LuaExt_ValidationResult___construct, 0, 0, 0)
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, valid, _IS_BOOL, 0, "true")
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, message, IS_STRING, 1, "null")
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, line, IS_LONG, 1, "null")
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, chunkName, IS_STRING, 1, "null")
ZEND_END_ARG_INFO()

#define arginfo_class_DevelopGravity_LuaExt_ValidationResult_jsonSerialize arginfo_class_DevelopGravity_LuaExt_SandboxStats_jsonSerialize

ZEND_BEGIN_ARG_INFO_EX(arginfo_class_DevelopGravity_LuaExt_Sandbox___construct, 0, 0, 0)
	ZEND_ARG_OBJ_INFO_WITH_DEFAULT_VALUE(0, config, DevelopGravity\\LuaExt\\SandboxConfig, 1, "null")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_DevelopGravity_LuaExt_Sandbox_extensionVersion, 0, 0, IS_STRING, 0)
ZEND_END_ARG_INFO()

#define arginfo_class_DevelopGravity_LuaExt_Sandbox_luaVersion arginfo_class_DevelopGravity_LuaExt_Sandbox_extensionVersion

#define arginfo_class_DevelopGravity_LuaExt_Sandbox_features arginfo_class_DevelopGravity_LuaExt_SandboxStats_jsonSerialize

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_class_DevelopGravity_LuaExt_Sandbox_compile, 0, 1, DevelopGravity\\LuaExt\\LuaFunction, 0)
	ZEND_ARG_TYPE_INFO(0, code, IS_STRING, 0)
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, chunkName, IS_STRING, 0, "\'=(load)\'")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_class_DevelopGravity_LuaExt_Sandbox_validate, 0, 1, DevelopGravity\\LuaExt\\ValidationResult, 0)
	ZEND_ARG_TYPE_INFO(0, code, IS_STRING, 0)
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, chunkName, IS_STRING, 0, "\'=(load)\'")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_class_DevelopGravity_LuaExt_Sandbox_compileBinary, 0, 1, DevelopGravity\\LuaExt\\LuaFunction, 0)
	ZEND_ARG_TYPE_INFO(0, bytecode, IS_STRING, 0)
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, chunkName, IS_STRING, 0, "\'=(binary)\'")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_DevelopGravity_LuaExt_Sandbox_eval, 0, 1, IS_ARRAY, 0)
	ZEND_ARG_TYPE_INFO(0, code, IS_STRING, 0)
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, chunkName, IS_STRING, 0, "\'=(eval)\'")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_DevelopGravity_LuaExt_Sandbox_call, 0, 1, IS_ARRAY, 0)
	ZEND_ARG_TYPE_INFO(0, path, IS_STRING, 0)
	ZEND_ARG_VARIADIC_TYPE_INFO(0, args, IS_MIXED, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_DevelopGravity_LuaExt_Sandbox_getGlobal, 0, 1, IS_MIXED, 0)
	ZEND_ARG_TYPE_INFO(0, path, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_DevelopGravity_LuaExt_Sandbox_setGlobal, 0, 2, IS_VOID, 0)
	ZEND_ARG_TYPE_INFO(0, path, IS_STRING, 0)
	ZEND_ARG_TYPE_INFO(0, value, IS_MIXED, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_class_DevelopGravity_LuaExt_Sandbox_wrapCallable, 0, 1, DevelopGravity\\LuaExt\\LuaFunction, 0)
	ZEND_ARG_TYPE_INFO(0, callback, IS_CALLABLE, 0)
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, name, IS_STRING, 1, "null")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_DevelopGravity_LuaExt_Sandbox_registerLibrary, 0, 2, IS_VOID, 0)
	ZEND_ARG_TYPE_INFO(0, name, IS_STRING, 0)
	ZEND_ARG_TYPE_INFO(0, functions, IS_ARRAY, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_DevelopGravity_LuaExt_Sandbox_registerObject, 0, 2, IS_VOID, 0)
	ZEND_ARG_TYPE_INFO(0, name, IS_STRING, 0)
	ZEND_ARG_TYPE_INFO(0, instance, IS_OBJECT, 0)
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, methods, IS_ARRAY, 1, "null")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_DevelopGravity_LuaExt_Sandbox_preloadModule, 0, 2, IS_VOID, 0)
	ZEND_ARG_TYPE_INFO(0, name, IS_STRING, 0)
	ZEND_ARG_OBJ_TYPE_MASK(0, loader, DevelopGravity\\LuaExt\\LuaFunction, MAY_BE_CALLABLE, NULL)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_DevelopGravity_LuaExt_Sandbox_setMemoryLimit, 0, 1, IS_VOID, 0)
	ZEND_ARG_TYPE_INFO(0, bytes, IS_LONG, 1)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_DevelopGravity_LuaExt_Sandbox_setCpuLimit, 0, 1, IS_VOID, 0)
	ZEND_ARG_TYPE_INFO(0, seconds, IS_DOUBLE, 1)
ZEND_END_ARG_INFO()

#define arginfo_class_DevelopGravity_LuaExt_Sandbox_setWallClockLimit arginfo_class_DevelopGravity_LuaExt_Sandbox_setCpuLimit

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_DevelopGravity_LuaExt_Sandbox_pauseTimers, 0, 0, _IS_BOOL, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_DevelopGravity_LuaExt_Sandbox_resumeTimers, 0, 0, IS_VOID, 0)
ZEND_END_ARG_INFO()

#define arginfo_class_DevelopGravity_LuaExt_Sandbox_interrupt arginfo_class_DevelopGravity_LuaExt_Sandbox_resumeTimers

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_class_DevelopGravity_LuaExt_Sandbox_stats, 0, 0, DevelopGravity\\LuaExt\\SandboxStats, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_DevelopGravity_LuaExt_Sandbox_getMemoryUsage, 0, 0, IS_LONG, 0)
ZEND_END_ARG_INFO()

#define arginfo_class_DevelopGravity_LuaExt_Sandbox_getPeakMemoryUsage arginfo_class_DevelopGravity_LuaExt_Sandbox_getMemoryUsage

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_DevelopGravity_LuaExt_Sandbox_getCpuUsage, 0, 0, IS_DOUBLE, 0)
ZEND_END_ARG_INFO()

#define arginfo_class_DevelopGravity_LuaExt_Sandbox_getWallClockUsage arginfo_class_DevelopGravity_LuaExt_Sandbox_getCpuUsage

#define arginfo_class_DevelopGravity_LuaExt_Sandbox_getOutput arginfo_class_DevelopGravity_LuaExt_Sandbox_extensionVersion

#define arginfo_class_DevelopGravity_LuaExt_Sandbox_takeOutput arginfo_class_DevelopGravity_LuaExt_Sandbox_extensionVersion

#define arginfo_class_DevelopGravity_LuaExt_Sandbox_getOutputLength arginfo_class_DevelopGravity_LuaExt_Sandbox_getMemoryUsage

#define arginfo_class_DevelopGravity_LuaExt_Sandbox_isOutputTruncated arginfo_class_DevelopGravity_LuaExt_Sandbox_pauseTimers

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_DevelopGravity_LuaExt_Sandbox_enableProfiler, 0, 0, _IS_BOOL, 0)
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, periodSeconds, IS_DOUBLE, 0, "0.002")
ZEND_END_ARG_INFO()

#define arginfo_class_DevelopGravity_LuaExt_Sandbox_disableProfiler arginfo_class_DevelopGravity_LuaExt_Sandbox_resumeTimers

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_DevelopGravity_LuaExt_Sandbox_getProfile, 0, 0, IS_ARRAY, 0)
	ZEND_ARG_OBJ_INFO_WITH_DEFAULT_VALUE(0, unit, DevelopGravity\\LuaExt\\ProfilerUnit, 0, "DevelopGravity\\LuaExt\\ProfilerUnit::Seconds")
ZEND_END_ARG_INFO()

#define arginfo_class_DevelopGravity_LuaExt_Sandbox_close arginfo_class_DevelopGravity_LuaExt_Sandbox_resumeTimers

#define arginfo_class_DevelopGravity_LuaExt_Sandbox_isClosed arginfo_class_DevelopGravity_LuaExt_Sandbox_pauseTimers

#define arginfo_class_DevelopGravity_LuaExt_LuaFunction___construct arginfo_class_DevelopGravity_LuaExt_SandboxStats___construct

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_DevelopGravity_LuaExt_LuaFunction_call, 0, 0, IS_ARRAY, 0)
	ZEND_ARG_VARIADIC_TYPE_INFO(0, args, IS_MIXED, 0)
ZEND_END_ARG_INFO()

#define arginfo_class_DevelopGravity_LuaExt_LuaFunction___invoke arginfo_class_DevelopGravity_LuaExt_LuaFunction_call

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_DevelopGravity_LuaExt_LuaFunction_dump, 0, 0, IS_STRING, 0)
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, strip, _IS_BOOL, 0, "true")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_class_DevelopGravity_LuaExt_LuaFunction_getSandbox, 0, 0, DevelopGravity\\LuaExt\\Sandbox, 0)
ZEND_END_ARG_INFO()

#define arginfo_class_DevelopGravity_LuaExt_LuaFunction_isValid arginfo_class_DevelopGravity_LuaExt_Sandbox_pauseTimers

ZEND_BEGIN_ARG_INFO_EX(arginfo_class_DevelopGravity_LuaExt_FileStat___construct, 0, 0, 2)
	ZEND_ARG_TYPE_INFO(0, size, IS_LONG, 0)
	ZEND_ARG_TYPE_INFO(0, mtime, IS_LONG, 0)
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, isDirectory, _IS_BOOL, 0, "false")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_DevelopGravity_LuaExt_FileSystem_exists, 0, 1, _IS_BOOL, 0)
	ZEND_ARG_TYPE_INFO(0, path, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_class_DevelopGravity_LuaExt_FileSystem_stat, 0, 1, DevelopGravity\\LuaExt\\FileStat, 1)
	ZEND_ARG_TYPE_INFO(0, path, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_DevelopGravity_LuaExt_FileSystem_read, 0, 1, IS_STRING, 0)
	ZEND_ARG_TYPE_INFO(0, path, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_DevelopGravity_LuaExt_FileSystem_write, 0, 2, IS_VOID, 0)
	ZEND_ARG_TYPE_INFO(0, path, IS_STRING, 0)
	ZEND_ARG_TYPE_INFO(0, contents, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_DevelopGravity_LuaExt_FileSystem_delete, 0, 1, IS_VOID, 0)
	ZEND_ARG_TYPE_INFO(0, path, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_DevelopGravity_LuaExt_FileSystem_rename, 0, 2, IS_VOID, 0)
	ZEND_ARG_TYPE_INFO(0, from, IS_STRING, 0)
	ZEND_ARG_TYPE_INFO(0, to, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_DevelopGravity_LuaExt_FileSystem_list, 0, 1, IS_ARRAY, 0)
	ZEND_ARG_TYPE_INFO(0, path, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_DevelopGravity_LuaExt_RangedFileSystem_readRange, 0, 3, IS_STRING, 0)
	ZEND_ARG_TYPE_INFO(0, path, IS_STRING, 0)
	ZEND_ARG_TYPE_INFO(0, offset, IS_LONG, 0)
	ZEND_ARG_TYPE_INFO(0, length, IS_LONG, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_DevelopGravity_LuaExt_RangedFileSystem_writeRange, 0, 3, IS_VOID, 0)
	ZEND_ARG_TYPE_INFO(0, path, IS_STRING, 0)
	ZEND_ARG_TYPE_INFO(0, offset, IS_LONG, 0)
	ZEND_ARG_TYPE_INFO(0, data, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_DevelopGravity_LuaExt_RangedFileSystem_truncate, 0, 2, IS_VOID, 0)
	ZEND_ARG_TYPE_INFO(0, path, IS_STRING, 0)
	ZEND_ARG_TYPE_INFO(0, size, IS_LONG, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_class_DevelopGravity_LuaExt_ModuleSource___construct, 0, 0, 2)
	ZEND_ARG_TYPE_INFO(0, code, IS_STRING, 0)
	ZEND_ARG_TYPE_INFO(0, chunkName, IS_STRING, 0)
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, isBytecode, _IS_BOOL, 0, "false")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_class_DevelopGravity_LuaExt_ModuleResolver_resolve, 0, 2, DevelopGravity\\LuaExt\\ModuleSource, 1)
	ZEND_ARG_TYPE_INFO(0, module, IS_STRING, 0)
	ZEND_ARG_TYPE_INFO(0, requestedBy, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_METHOD(DevelopGravity_LuaExt_LuaMethod, __construct);
ZEND_METHOD(DevelopGravity_LuaExt_Capabilities, __construct);
ZEND_METHOD(DevelopGravity_LuaExt_Capabilities, untrusted);
ZEND_METHOD(DevelopGravity_LuaExt_Capabilities, trusted);
ZEND_METHOD(DevelopGravity_LuaExt_Capabilities, with);
ZEND_METHOD(DevelopGravity_LuaExt_Limits, __construct);
ZEND_METHOD(DevelopGravity_LuaExt_Limits, with);
ZEND_METHOD(DevelopGravity_LuaExt_VfsQuota, __construct);
ZEND_METHOD(DevelopGravity_LuaExt_VfsQuota, with);
ZEND_METHOD(DevelopGravity_LuaExt_SandboxConfig, __construct);
ZEND_METHOD(DevelopGravity_LuaExt_SandboxConfig, with);
ZEND_METHOD(DevelopGravity_LuaExt_SandboxStats, __construct);
ZEND_METHOD(DevelopGravity_LuaExt_SandboxStats, jsonSerialize);
ZEND_METHOD(DevelopGravity_LuaExt_ValidationResult, __construct);
ZEND_METHOD(DevelopGravity_LuaExt_ValidationResult, jsonSerialize);
ZEND_METHOD(DevelopGravity_LuaExt_Sandbox, __construct);
ZEND_METHOD(DevelopGravity_LuaExt_Sandbox, extensionVersion);
ZEND_METHOD(DevelopGravity_LuaExt_Sandbox, luaVersion);
ZEND_METHOD(DevelopGravity_LuaExt_Sandbox, features);
ZEND_METHOD(DevelopGravity_LuaExt_Sandbox, compile);
ZEND_METHOD(DevelopGravity_LuaExt_Sandbox, validate);
ZEND_METHOD(DevelopGravity_LuaExt_Sandbox, compileBinary);
ZEND_METHOD(DevelopGravity_LuaExt_Sandbox, eval);
ZEND_METHOD(DevelopGravity_LuaExt_Sandbox, call);
ZEND_METHOD(DevelopGravity_LuaExt_Sandbox, getGlobal);
ZEND_METHOD(DevelopGravity_LuaExt_Sandbox, setGlobal);
ZEND_METHOD(DevelopGravity_LuaExt_Sandbox, wrapCallable);
ZEND_METHOD(DevelopGravity_LuaExt_Sandbox, registerLibrary);
ZEND_METHOD(DevelopGravity_LuaExt_Sandbox, registerObject);
ZEND_METHOD(DevelopGravity_LuaExt_Sandbox, preloadModule);
ZEND_METHOD(DevelopGravity_LuaExt_Sandbox, setMemoryLimit);
ZEND_METHOD(DevelopGravity_LuaExt_Sandbox, setCpuLimit);
ZEND_METHOD(DevelopGravity_LuaExt_Sandbox, setWallClockLimit);
ZEND_METHOD(DevelopGravity_LuaExt_Sandbox, pauseTimers);
ZEND_METHOD(DevelopGravity_LuaExt_Sandbox, resumeTimers);
ZEND_METHOD(DevelopGravity_LuaExt_Sandbox, interrupt);
ZEND_METHOD(DevelopGravity_LuaExt_Sandbox, stats);
ZEND_METHOD(DevelopGravity_LuaExt_Sandbox, getMemoryUsage);
ZEND_METHOD(DevelopGravity_LuaExt_Sandbox, getPeakMemoryUsage);
ZEND_METHOD(DevelopGravity_LuaExt_Sandbox, getCpuUsage);
ZEND_METHOD(DevelopGravity_LuaExt_Sandbox, getWallClockUsage);
ZEND_METHOD(DevelopGravity_LuaExt_Sandbox, getOutput);
ZEND_METHOD(DevelopGravity_LuaExt_Sandbox, takeOutput);
ZEND_METHOD(DevelopGravity_LuaExt_Sandbox, getOutputLength);
ZEND_METHOD(DevelopGravity_LuaExt_Sandbox, isOutputTruncated);
ZEND_METHOD(DevelopGravity_LuaExt_Sandbox, enableProfiler);
ZEND_METHOD(DevelopGravity_LuaExt_Sandbox, disableProfiler);
ZEND_METHOD(DevelopGravity_LuaExt_Sandbox, getProfile);
ZEND_METHOD(DevelopGravity_LuaExt_Sandbox, close);
ZEND_METHOD(DevelopGravity_LuaExt_Sandbox, isClosed);
ZEND_METHOD(DevelopGravity_LuaExt_LuaFunction, __construct);
ZEND_METHOD(DevelopGravity_LuaExt_LuaFunction, call);
ZEND_METHOD(DevelopGravity_LuaExt_LuaFunction, __invoke);
ZEND_METHOD(DevelopGravity_LuaExt_LuaFunction, dump);
ZEND_METHOD(DevelopGravity_LuaExt_LuaFunction, getSandbox);
ZEND_METHOD(DevelopGravity_LuaExt_LuaFunction, isValid);
ZEND_METHOD(DevelopGravity_LuaExt_FileStat, __construct);
ZEND_METHOD(DevelopGravity_LuaExt_ModuleSource, __construct);

static const zend_function_entry class_DevelopGravity_LuaExt_LuaMethod_methods[] = {
	ZEND_ME(DevelopGravity_LuaExt_LuaMethod, __construct, arginfo_class_DevelopGravity_LuaExt_LuaMethod___construct, ZEND_ACC_PUBLIC)
	ZEND_FE_END
};

static const zend_function_entry class_DevelopGravity_LuaExt_Capabilities_methods[] = {
	ZEND_ME(DevelopGravity_LuaExt_Capabilities, __construct, arginfo_class_DevelopGravity_LuaExt_Capabilities___construct, ZEND_ACC_PUBLIC)
	ZEND_ME(DevelopGravity_LuaExt_Capabilities, untrusted, arginfo_class_DevelopGravity_LuaExt_Capabilities_untrusted, ZEND_ACC_PUBLIC|ZEND_ACC_STATIC)
	ZEND_ME(DevelopGravity_LuaExt_Capabilities, trusted, arginfo_class_DevelopGravity_LuaExt_Capabilities_trusted, ZEND_ACC_PUBLIC|ZEND_ACC_STATIC)
	ZEND_ME(DevelopGravity_LuaExt_Capabilities, with, arginfo_class_DevelopGravity_LuaExt_Capabilities_with, ZEND_ACC_PUBLIC)
	ZEND_FE_END
};

static const zend_function_entry class_DevelopGravity_LuaExt_Limits_methods[] = {
	ZEND_ME(DevelopGravity_LuaExt_Limits, __construct, arginfo_class_DevelopGravity_LuaExt_Limits___construct, ZEND_ACC_PUBLIC)
	ZEND_ME(DevelopGravity_LuaExt_Limits, with, arginfo_class_DevelopGravity_LuaExt_Limits_with, ZEND_ACC_PUBLIC)
	ZEND_FE_END
};

static const zend_function_entry class_DevelopGravity_LuaExt_VfsQuota_methods[] = {
	ZEND_ME(DevelopGravity_LuaExt_VfsQuota, __construct, arginfo_class_DevelopGravity_LuaExt_VfsQuota___construct, ZEND_ACC_PUBLIC)
	ZEND_ME(DevelopGravity_LuaExt_VfsQuota, with, arginfo_class_DevelopGravity_LuaExt_VfsQuota_with, ZEND_ACC_PUBLIC)
	ZEND_FE_END
};

static const zend_function_entry class_DevelopGravity_LuaExt_SandboxConfig_methods[] = {
	ZEND_ME(DevelopGravity_LuaExt_SandboxConfig, __construct, arginfo_class_DevelopGravity_LuaExt_SandboxConfig___construct, ZEND_ACC_PUBLIC)
	ZEND_ME(DevelopGravity_LuaExt_SandboxConfig, with, arginfo_class_DevelopGravity_LuaExt_SandboxConfig_with, ZEND_ACC_PUBLIC)
	ZEND_FE_END
};

static const zend_function_entry class_DevelopGravity_LuaExt_SandboxStats_methods[] = {
	ZEND_ME(DevelopGravity_LuaExt_SandboxStats, __construct, arginfo_class_DevelopGravity_LuaExt_SandboxStats___construct, ZEND_ACC_PRIVATE)
	ZEND_ME(DevelopGravity_LuaExt_SandboxStats, jsonSerialize, arginfo_class_DevelopGravity_LuaExt_SandboxStats_jsonSerialize, ZEND_ACC_PUBLIC)
	ZEND_FE_END
};

static const zend_function_entry class_DevelopGravity_LuaExt_ValidationResult_methods[] = {
	ZEND_ME(DevelopGravity_LuaExt_ValidationResult, __construct, arginfo_class_DevelopGravity_LuaExt_ValidationResult___construct, ZEND_ACC_PUBLIC)
	ZEND_ME(DevelopGravity_LuaExt_ValidationResult, jsonSerialize, arginfo_class_DevelopGravity_LuaExt_ValidationResult_jsonSerialize, ZEND_ACC_PUBLIC)
	ZEND_FE_END
};

static const zend_function_entry class_DevelopGravity_LuaExt_Sandbox_methods[] = {
	ZEND_ME(DevelopGravity_LuaExt_Sandbox, __construct, arginfo_class_DevelopGravity_LuaExt_Sandbox___construct, ZEND_ACC_PUBLIC)
	ZEND_ME(DevelopGravity_LuaExt_Sandbox, extensionVersion, arginfo_class_DevelopGravity_LuaExt_Sandbox_extensionVersion, ZEND_ACC_PUBLIC|ZEND_ACC_STATIC)
	ZEND_ME(DevelopGravity_LuaExt_Sandbox, luaVersion, arginfo_class_DevelopGravity_LuaExt_Sandbox_luaVersion, ZEND_ACC_PUBLIC|ZEND_ACC_STATIC)
	ZEND_ME(DevelopGravity_LuaExt_Sandbox, features, arginfo_class_DevelopGravity_LuaExt_Sandbox_features, ZEND_ACC_PUBLIC|ZEND_ACC_STATIC)
	ZEND_ME(DevelopGravity_LuaExt_Sandbox, compile, arginfo_class_DevelopGravity_LuaExt_Sandbox_compile, ZEND_ACC_PUBLIC)
	ZEND_ME(DevelopGravity_LuaExt_Sandbox, validate, arginfo_class_DevelopGravity_LuaExt_Sandbox_validate, ZEND_ACC_PUBLIC|ZEND_ACC_NODISCARD)
	ZEND_ME(DevelopGravity_LuaExt_Sandbox, compileBinary, arginfo_class_DevelopGravity_LuaExt_Sandbox_compileBinary, ZEND_ACC_PUBLIC)
	ZEND_ME(DevelopGravity_LuaExt_Sandbox, eval, arginfo_class_DevelopGravity_LuaExt_Sandbox_eval, ZEND_ACC_PUBLIC|ZEND_ACC_NODISCARD)
	ZEND_ME(DevelopGravity_LuaExt_Sandbox, call, arginfo_class_DevelopGravity_LuaExt_Sandbox_call, ZEND_ACC_PUBLIC|ZEND_ACC_NODISCARD)
	ZEND_ME(DevelopGravity_LuaExt_Sandbox, getGlobal, arginfo_class_DevelopGravity_LuaExt_Sandbox_getGlobal, ZEND_ACC_PUBLIC)
	ZEND_ME(DevelopGravity_LuaExt_Sandbox, setGlobal, arginfo_class_DevelopGravity_LuaExt_Sandbox_setGlobal, ZEND_ACC_PUBLIC)
	ZEND_ME(DevelopGravity_LuaExt_Sandbox, wrapCallable, arginfo_class_DevelopGravity_LuaExt_Sandbox_wrapCallable, ZEND_ACC_PUBLIC)
	ZEND_ME(DevelopGravity_LuaExt_Sandbox, registerLibrary, arginfo_class_DevelopGravity_LuaExt_Sandbox_registerLibrary, ZEND_ACC_PUBLIC)
	ZEND_ME(DevelopGravity_LuaExt_Sandbox, registerObject, arginfo_class_DevelopGravity_LuaExt_Sandbox_registerObject, ZEND_ACC_PUBLIC)
	ZEND_ME(DevelopGravity_LuaExt_Sandbox, preloadModule, arginfo_class_DevelopGravity_LuaExt_Sandbox_preloadModule, ZEND_ACC_PUBLIC)
	ZEND_ME(DevelopGravity_LuaExt_Sandbox, setMemoryLimit, arginfo_class_DevelopGravity_LuaExt_Sandbox_setMemoryLimit, ZEND_ACC_PUBLIC)
	ZEND_ME(DevelopGravity_LuaExt_Sandbox, setCpuLimit, arginfo_class_DevelopGravity_LuaExt_Sandbox_setCpuLimit, ZEND_ACC_PUBLIC)
	ZEND_ME(DevelopGravity_LuaExt_Sandbox, setWallClockLimit, arginfo_class_DevelopGravity_LuaExt_Sandbox_setWallClockLimit, ZEND_ACC_PUBLIC)
	ZEND_ME(DevelopGravity_LuaExt_Sandbox, pauseTimers, arginfo_class_DevelopGravity_LuaExt_Sandbox_pauseTimers, ZEND_ACC_PUBLIC)
	ZEND_ME(DevelopGravity_LuaExt_Sandbox, resumeTimers, arginfo_class_DevelopGravity_LuaExt_Sandbox_resumeTimers, ZEND_ACC_PUBLIC)
	ZEND_ME(DevelopGravity_LuaExt_Sandbox, interrupt, arginfo_class_DevelopGravity_LuaExt_Sandbox_interrupt, ZEND_ACC_PUBLIC)
	ZEND_ME(DevelopGravity_LuaExt_Sandbox, stats, arginfo_class_DevelopGravity_LuaExt_Sandbox_stats, ZEND_ACC_PUBLIC)
	ZEND_ME(DevelopGravity_LuaExt_Sandbox, getMemoryUsage, arginfo_class_DevelopGravity_LuaExt_Sandbox_getMemoryUsage, ZEND_ACC_PUBLIC)
	ZEND_ME(DevelopGravity_LuaExt_Sandbox, getPeakMemoryUsage, arginfo_class_DevelopGravity_LuaExt_Sandbox_getPeakMemoryUsage, ZEND_ACC_PUBLIC)
	ZEND_ME(DevelopGravity_LuaExt_Sandbox, getCpuUsage, arginfo_class_DevelopGravity_LuaExt_Sandbox_getCpuUsage, ZEND_ACC_PUBLIC)
	ZEND_ME(DevelopGravity_LuaExt_Sandbox, getWallClockUsage, arginfo_class_DevelopGravity_LuaExt_Sandbox_getWallClockUsage, ZEND_ACC_PUBLIC)
	ZEND_ME(DevelopGravity_LuaExt_Sandbox, getOutput, arginfo_class_DevelopGravity_LuaExt_Sandbox_getOutput, ZEND_ACC_PUBLIC)
	ZEND_ME(DevelopGravity_LuaExt_Sandbox, takeOutput, arginfo_class_DevelopGravity_LuaExt_Sandbox_takeOutput, ZEND_ACC_PUBLIC|ZEND_ACC_NODISCARD)
	ZEND_ME(DevelopGravity_LuaExt_Sandbox, getOutputLength, arginfo_class_DevelopGravity_LuaExt_Sandbox_getOutputLength, ZEND_ACC_PUBLIC)
	ZEND_ME(DevelopGravity_LuaExt_Sandbox, isOutputTruncated, arginfo_class_DevelopGravity_LuaExt_Sandbox_isOutputTruncated, ZEND_ACC_PUBLIC)
	ZEND_ME(DevelopGravity_LuaExt_Sandbox, enableProfiler, arginfo_class_DevelopGravity_LuaExt_Sandbox_enableProfiler, ZEND_ACC_PUBLIC)
	ZEND_ME(DevelopGravity_LuaExt_Sandbox, disableProfiler, arginfo_class_DevelopGravity_LuaExt_Sandbox_disableProfiler, ZEND_ACC_PUBLIC)
	ZEND_ME(DevelopGravity_LuaExt_Sandbox, getProfile, arginfo_class_DevelopGravity_LuaExt_Sandbox_getProfile, ZEND_ACC_PUBLIC)
	ZEND_ME(DevelopGravity_LuaExt_Sandbox, close, arginfo_class_DevelopGravity_LuaExt_Sandbox_close, ZEND_ACC_PUBLIC)
	ZEND_ME(DevelopGravity_LuaExt_Sandbox, isClosed, arginfo_class_DevelopGravity_LuaExt_Sandbox_isClosed, ZEND_ACC_PUBLIC)
	ZEND_FE_END
};

static const zend_function_entry class_DevelopGravity_LuaExt_LuaFunction_methods[] = {
	ZEND_ME(DevelopGravity_LuaExt_LuaFunction, __construct, arginfo_class_DevelopGravity_LuaExt_LuaFunction___construct, ZEND_ACC_PRIVATE)
	ZEND_ME(DevelopGravity_LuaExt_LuaFunction, call, arginfo_class_DevelopGravity_LuaExt_LuaFunction_call, ZEND_ACC_PUBLIC|ZEND_ACC_NODISCARD)
	ZEND_ME(DevelopGravity_LuaExt_LuaFunction, __invoke, arginfo_class_DevelopGravity_LuaExt_LuaFunction___invoke, ZEND_ACC_PUBLIC|ZEND_ACC_NODISCARD)
	ZEND_ME(DevelopGravity_LuaExt_LuaFunction, dump, arginfo_class_DevelopGravity_LuaExt_LuaFunction_dump, ZEND_ACC_PUBLIC)
	ZEND_ME(DevelopGravity_LuaExt_LuaFunction, getSandbox, arginfo_class_DevelopGravity_LuaExt_LuaFunction_getSandbox, ZEND_ACC_PUBLIC)
	ZEND_ME(DevelopGravity_LuaExt_LuaFunction, isValid, arginfo_class_DevelopGravity_LuaExt_LuaFunction_isValid, ZEND_ACC_PUBLIC)
	ZEND_FE_END
};

static const zend_function_entry class_DevelopGravity_LuaExt_FileStat_methods[] = {
	ZEND_ME(DevelopGravity_LuaExt_FileStat, __construct, arginfo_class_DevelopGravity_LuaExt_FileStat___construct, ZEND_ACC_PUBLIC)
	ZEND_FE_END
};

static const zend_function_entry class_DevelopGravity_LuaExt_FileSystem_methods[] = {
	ZEND_RAW_FENTRY("exists", NULL, arginfo_class_DevelopGravity_LuaExt_FileSystem_exists, ZEND_ACC_PUBLIC|ZEND_ACC_ABSTRACT, NULL, NULL)
	ZEND_RAW_FENTRY("stat", NULL, arginfo_class_DevelopGravity_LuaExt_FileSystem_stat, ZEND_ACC_PUBLIC|ZEND_ACC_ABSTRACT, NULL, NULL)
	ZEND_RAW_FENTRY("read", NULL, arginfo_class_DevelopGravity_LuaExt_FileSystem_read, ZEND_ACC_PUBLIC|ZEND_ACC_ABSTRACT, NULL, NULL)
	ZEND_RAW_FENTRY("write", NULL, arginfo_class_DevelopGravity_LuaExt_FileSystem_write, ZEND_ACC_PUBLIC|ZEND_ACC_ABSTRACT, NULL, NULL)
	ZEND_RAW_FENTRY("delete", NULL, arginfo_class_DevelopGravity_LuaExt_FileSystem_delete, ZEND_ACC_PUBLIC|ZEND_ACC_ABSTRACT, NULL, NULL)
	ZEND_RAW_FENTRY("rename", NULL, arginfo_class_DevelopGravity_LuaExt_FileSystem_rename, ZEND_ACC_PUBLIC|ZEND_ACC_ABSTRACT, NULL, NULL)
	ZEND_RAW_FENTRY("list", NULL, arginfo_class_DevelopGravity_LuaExt_FileSystem_list, ZEND_ACC_PUBLIC|ZEND_ACC_ABSTRACT, NULL, NULL)
	ZEND_FE_END
};

static const zend_function_entry class_DevelopGravity_LuaExt_RangedFileSystem_methods[] = {
	ZEND_RAW_FENTRY("readRange", NULL, arginfo_class_DevelopGravity_LuaExt_RangedFileSystem_readRange, ZEND_ACC_PUBLIC|ZEND_ACC_ABSTRACT, NULL, NULL)
	ZEND_RAW_FENTRY("writeRange", NULL, arginfo_class_DevelopGravity_LuaExt_RangedFileSystem_writeRange, ZEND_ACC_PUBLIC|ZEND_ACC_ABSTRACT, NULL, NULL)
	ZEND_RAW_FENTRY("truncate", NULL, arginfo_class_DevelopGravity_LuaExt_RangedFileSystem_truncate, ZEND_ACC_PUBLIC|ZEND_ACC_ABSTRACT, NULL, NULL)
	ZEND_FE_END
};

static const zend_function_entry class_DevelopGravity_LuaExt_ModuleSource_methods[] = {
	ZEND_ME(DevelopGravity_LuaExt_ModuleSource, __construct, arginfo_class_DevelopGravity_LuaExt_ModuleSource___construct, ZEND_ACC_PUBLIC)
	ZEND_FE_END
};

static const zend_function_entry class_DevelopGravity_LuaExt_ModuleResolver_methods[] = {
	ZEND_RAW_FENTRY("resolve", NULL, arginfo_class_DevelopGravity_LuaExt_ModuleResolver_resolve, ZEND_ACC_PUBLIC|ZEND_ACC_ABSTRACT, NULL, NULL)
	ZEND_FE_END
};

static zend_class_entry *register_class_DevelopGravity_LuaExt_OutputMode(void)
{
	zend_class_entry *class_entry = zend_register_internal_enum("DevelopGravity\\LuaExt\\OutputMode", IS_UNDEF, NULL);

	zend_enum_add_case_cstr(class_entry, "Buffer", NULL);

	zend_enum_add_case_cstr(class_entry, "Callback", NULL);

	zend_enum_add_case_cstr(class_entry, "Discard", NULL);

	return class_entry;
}

static zend_class_entry *register_class_DevelopGravity_LuaExt_OverflowBehavior(void)
{
	zend_class_entry *class_entry = zend_register_internal_enum("DevelopGravity\\LuaExt\\OverflowBehavior", IS_UNDEF, NULL);

	zend_enum_add_case_cstr(class_entry, "Truncate", NULL);

	zend_enum_add_case_cstr(class_entry, "Fail", NULL);

	return class_entry;
}

static zend_class_entry *register_class_DevelopGravity_LuaExt_ProfilerUnit(void)
{
	zend_class_entry *class_entry = zend_register_internal_enum("DevelopGravity\\LuaExt\\ProfilerUnit", IS_UNDEF, NULL);

	zend_enum_add_case_cstr(class_entry, "Samples", NULL);

	zend_enum_add_case_cstr(class_entry, "Seconds", NULL);

	zend_enum_add_case_cstr(class_entry, "Percent", NULL);

	return class_entry;
}

static zend_class_entry *register_class_DevelopGravity_LuaExt_LimitSupport(void)
{
	zend_class_entry *class_entry = zend_register_internal_enum("DevelopGravity\\LuaExt\\LimitSupport", IS_UNDEF, NULL);

	zend_enum_add_case_cstr(class_entry, "Enforced", NULL);

	zend_enum_add_case_cstr(class_entry, "Degraded", NULL);

	zend_enum_add_case_cstr(class_entry, "Unsupported", NULL);

	return class_entry;
}

static zend_class_entry *register_class_DevelopGravity_LuaExt_SealMode(void)
{
	zend_class_entry *class_entry = zend_register_internal_enum("DevelopGravity\\LuaExt\\SealMode", IS_UNDEF, NULL);

	zend_enum_add_case_cstr(class_entry, "Checksum", NULL);

	zend_enum_add_case_cstr(class_entry, "Authenticated", NULL);

	return class_entry;
}

static zend_class_entry *register_class_DevelopGravity_LuaExt_LuaMethod(void)
{
	zend_class_entry ce, *class_entry;

	INIT_NS_CLASS_ENTRY(ce, "DevelopGravity\\LuaExt", "LuaMethod", class_DevelopGravity_LuaExt_LuaMethod_methods);
	class_entry = zend_register_internal_class_with_flags(&ce, NULL, ZEND_ACC_FINAL);

	zval property_name_default_value;
	ZVAL_UNDEF(&property_name_default_value);
	zend_declare_typed_property(class_entry, ZSTR_KNOWN(ZEND_STR_NAME), &property_name_default_value, ZEND_ACC_PUBLIC, NULL, (zend_type) ZEND_TYPE_INIT_MASK(MAY_BE_STRING|MAY_BE_NULL));

	return class_entry;
}

static zend_class_entry *register_class_DevelopGravity_LuaExt_Capabilities(void)
{
	zend_class_entry ce, *class_entry;

	INIT_NS_CLASS_ENTRY(ce, "DevelopGravity\\LuaExt", "Capabilities", class_DevelopGravity_LuaExt_Capabilities_methods);
	class_entry = zend_register_internal_class_with_flags(&ce, NULL, ZEND_ACC_FINAL|ZEND_ACC_NO_DYNAMIC_PROPERTIES|ZEND_ACC_READONLY_CLASS);

	zval property_loadBytecode_default_value;
	ZVAL_UNDEF(&property_loadBytecode_default_value);
	zend_string *property_loadBytecode_name = zend_string_init("loadBytecode", sizeof("loadBytecode") - 1, 1);
	zend_declare_typed_property(class_entry, property_loadBytecode_name, &property_loadBytecode_default_value, ZEND_ACC_PUBLIC|ZEND_ACC_READONLY, NULL, (zend_type) ZEND_TYPE_INIT_MASK(MAY_BE_BOOL));
	zend_string_release(property_loadBytecode_name);

	zval property_compileAtRuntime_default_value;
	ZVAL_UNDEF(&property_compileAtRuntime_default_value);
	zend_string *property_compileAtRuntime_name = zend_string_init("compileAtRuntime", sizeof("compileAtRuntime") - 1, 1);
	zend_declare_typed_property(class_entry, property_compileAtRuntime_name, &property_compileAtRuntime_default_value, ZEND_ACC_PUBLIC|ZEND_ACC_READONLY, NULL, (zend_type) ZEND_TYPE_INIT_MASK(MAY_BE_BOOL));
	zend_string_release(property_compileAtRuntime_name);

	zval property_dumpBytecode_default_value;
	ZVAL_UNDEF(&property_dumpBytecode_default_value);
	zend_string *property_dumpBytecode_name = zend_string_init("dumpBytecode", sizeof("dumpBytecode") - 1, 1);
	zend_declare_typed_property(class_entry, property_dumpBytecode_name, &property_dumpBytecode_default_value, ZEND_ACC_PUBLIC|ZEND_ACC_READONLY, NULL, (zend_type) ZEND_TYPE_INIT_MASK(MAY_BE_BOOL));
	zend_string_release(property_dumpBytecode_name);

	zval property_require_default_value;
	ZVAL_UNDEF(&property_require_default_value);
	zend_declare_typed_property(class_entry, ZSTR_KNOWN(ZEND_STR_REQUIRE), &property_require_default_value, ZEND_ACC_PUBLIC|ZEND_ACC_READONLY, NULL, (zend_type) ZEND_TYPE_INIT_MASK(MAY_BE_BOOL));

	zval property_vfs_default_value;
	ZVAL_UNDEF(&property_vfs_default_value);
	zend_string *property_vfs_name = zend_string_init("vfs", sizeof("vfs") - 1, 1);
	zend_declare_typed_property(class_entry, property_vfs_name, &property_vfs_default_value, ZEND_ACC_PUBLIC|ZEND_ACC_READONLY, NULL, (zend_type) ZEND_TYPE_INIT_MASK(MAY_BE_BOOL));
	zend_string_release(property_vfs_name);

	zval property_vfsWrite_default_value;
	ZVAL_UNDEF(&property_vfsWrite_default_value);
	zend_string *property_vfsWrite_name = zend_string_init("vfsWrite", sizeof("vfsWrite") - 1, 1);
	zend_declare_typed_property(class_entry, property_vfsWrite_name, &property_vfsWrite_default_value, ZEND_ACC_PUBLIC|ZEND_ACC_READONLY, NULL, (zend_type) ZEND_TYPE_INIT_MASK(MAY_BE_BOOL));
	zend_string_release(property_vfsWrite_name);

	zval property_coroutines_default_value;
	ZVAL_UNDEF(&property_coroutines_default_value);
	zend_string *property_coroutines_name = zend_string_init("coroutines", sizeof("coroutines") - 1, 1);
	zend_declare_typed_property(class_entry, property_coroutines_name, &property_coroutines_default_value, ZEND_ACC_PUBLIC|ZEND_ACC_READONLY, NULL, (zend_type) ZEND_TYPE_INIT_MASK(MAY_BE_BOOL));
	zend_string_release(property_coroutines_name);

	zval property_osTime_default_value;
	ZVAL_UNDEF(&property_osTime_default_value);
	zend_string *property_osTime_name = zend_string_init("osTime", sizeof("osTime") - 1, 1);
	zend_declare_typed_property(class_entry, property_osTime_name, &property_osTime_default_value, ZEND_ACC_PUBLIC|ZEND_ACC_READONLY, NULL, (zend_type) ZEND_TYPE_INIT_MASK(MAY_BE_BOOL));
	zend_string_release(property_osTime_name);

	zval property_osEnv_default_value;
	ZVAL_UNDEF(&property_osEnv_default_value);
	zend_string *property_osEnv_name = zend_string_init("osEnv", sizeof("osEnv") - 1, 1);
	zend_declare_typed_property(class_entry, property_osEnv_name, &property_osEnv_default_value, ZEND_ACC_PUBLIC|ZEND_ACC_READONLY, NULL, (zend_type) ZEND_TYPE_INIT_MASK(MAY_BE_BOOL));
	zend_string_release(property_osEnv_name);

	zval property_osEnvAllowList_default_value;
	ZVAL_UNDEF(&property_osEnvAllowList_default_value);
	zend_string *property_osEnvAllowList_name = zend_string_init("osEnvAllowList", sizeof("osEnvAllowList") - 1, 1);
	zend_declare_typed_property(class_entry, property_osEnvAllowList_name, &property_osEnvAllowList_default_value, ZEND_ACC_PUBLIC|ZEND_ACC_READONLY, NULL, (zend_type) ZEND_TYPE_INIT_MASK(MAY_BE_ARRAY));
	zend_string_release(property_osEnvAllowList_name);

	zval property_debugTraceback_default_value;
	ZVAL_UNDEF(&property_debugTraceback_default_value);
	zend_string *property_debugTraceback_name = zend_string_init("debugTraceback", sizeof("debugTraceback") - 1, 1);
	zend_declare_typed_property(class_entry, property_debugTraceback_name, &property_debugTraceback_default_value, ZEND_ACC_PUBLIC|ZEND_ACC_READONLY, NULL, (zend_type) ZEND_TYPE_INIT_MASK(MAY_BE_BOOL));
	zend_string_release(property_debugTraceback_name);

	zval property_debugIntrospect_default_value;
	ZVAL_UNDEF(&property_debugIntrospect_default_value);
	zend_string *property_debugIntrospect_name = zend_string_init("debugIntrospect", sizeof("debugIntrospect") - 1, 1);
	zend_declare_typed_property(class_entry, property_debugIntrospect_name, &property_debugIntrospect_default_value, ZEND_ACC_PUBLIC|ZEND_ACC_READONLY, NULL, (zend_type) ZEND_TYPE_INIT_MASK(MAY_BE_BOOL));
	zend_string_release(property_debugIntrospect_name);

	zval property_debugMutate_default_value;
	ZVAL_UNDEF(&property_debugMutate_default_value);
	zend_string *property_debugMutate_name = zend_string_init("debugMutate", sizeof("debugMutate") - 1, 1);
	zend_declare_typed_property(class_entry, property_debugMutate_name, &property_debugMutate_default_value, ZEND_ACC_PUBLIC|ZEND_ACC_READONLY, NULL, (zend_type) ZEND_TYPE_INIT_MASK(MAY_BE_BOOL));
	zend_string_release(property_debugMutate_name);

	zval property_debugHooks_default_value;
	ZVAL_UNDEF(&property_debugHooks_default_value);
	zend_string *property_debugHooks_name = zend_string_init("debugHooks", sizeof("debugHooks") - 1, 1);
	zend_declare_typed_property(class_entry, property_debugHooks_name, &property_debugHooks_default_value, ZEND_ACC_PUBLIC|ZEND_ACC_READONLY, NULL, (zend_type) ZEND_TYPE_INIT_MASK(MAY_BE_BOOL));
	zend_string_release(property_debugHooks_name);

	zval property_utf8_default_value;
	ZVAL_UNDEF(&property_utf8_default_value);
	zend_string *property_utf8_name = zend_string_init("utf8", sizeof("utf8") - 1, 1);
	zend_declare_typed_property(class_entry, property_utf8_name, &property_utf8_default_value, ZEND_ACC_PUBLIC|ZEND_ACC_READONLY, NULL, (zend_type) ZEND_TYPE_INIT_MASK(MAY_BE_BOOL));
	zend_string_release(property_utf8_name);

	zval property_gcControl_default_value;
	ZVAL_UNDEF(&property_gcControl_default_value);
	zend_string *property_gcControl_name = zend_string_init("gcControl", sizeof("gcControl") - 1, 1);
	zend_declare_typed_property(class_entry, property_gcControl_name, &property_gcControl_default_value, ZEND_ACC_PUBLIC|ZEND_ACC_READONLY, NULL, (zend_type) ZEND_TYPE_INIT_MASK(MAY_BE_BOOL));
	zend_string_release(property_gcControl_name);

	zval property_warn_default_value;
	ZVAL_UNDEF(&property_warn_default_value);
	zend_string *property_warn_name = zend_string_init("warn", sizeof("warn") - 1, 1);
	zend_declare_typed_property(class_entry, property_warn_name, &property_warn_default_value, ZEND_ACC_PUBLIC|ZEND_ACC_READONLY, NULL, (zend_type) ZEND_TYPE_INIT_MASK(MAY_BE_BOOL));
	zend_string_release(property_warn_name);

	return class_entry;
}

static zend_class_entry *register_class_DevelopGravity_LuaExt_Limits(void)
{
	zend_class_entry ce, *class_entry;

	INIT_NS_CLASS_ENTRY(ce, "DevelopGravity\\LuaExt", "Limits", class_DevelopGravity_LuaExt_Limits_methods);
	class_entry = zend_register_internal_class_with_flags(&ce, NULL, ZEND_ACC_FINAL|ZEND_ACC_NO_DYNAMIC_PROPERTIES|ZEND_ACC_READONLY_CLASS);

	zval property_memoryBytes_default_value;
	ZVAL_UNDEF(&property_memoryBytes_default_value);
	zend_string *property_memoryBytes_name = zend_string_init("memoryBytes", sizeof("memoryBytes") - 1, 1);
	zend_declare_typed_property(class_entry, property_memoryBytes_name, &property_memoryBytes_default_value, ZEND_ACC_PUBLIC|ZEND_ACC_READONLY, NULL, (zend_type) ZEND_TYPE_INIT_MASK(MAY_BE_LONG|MAY_BE_NULL));
	zend_string_release(property_memoryBytes_name);

	zval property_cpuSeconds_default_value;
	ZVAL_UNDEF(&property_cpuSeconds_default_value);
	zend_string *property_cpuSeconds_name = zend_string_init("cpuSeconds", sizeof("cpuSeconds") - 1, 1);
	zend_declare_typed_property(class_entry, property_cpuSeconds_name, &property_cpuSeconds_default_value, ZEND_ACC_PUBLIC|ZEND_ACC_READONLY, NULL, (zend_type) ZEND_TYPE_INIT_MASK(MAY_BE_DOUBLE|MAY_BE_NULL));
	zend_string_release(property_cpuSeconds_name);

	zval property_wallClockSeconds_default_value;
	ZVAL_UNDEF(&property_wallClockSeconds_default_value);
	zend_string *property_wallClockSeconds_name = zend_string_init("wallClockSeconds", sizeof("wallClockSeconds") - 1, 1);
	zend_declare_typed_property(class_entry, property_wallClockSeconds_name, &property_wallClockSeconds_default_value, ZEND_ACC_PUBLIC|ZEND_ACC_READONLY, NULL, (zend_type) ZEND_TYPE_INIT_MASK(MAY_BE_DOUBLE|MAY_BE_NULL));
	zend_string_release(property_wallClockSeconds_name);

	zval property_outputBytes_default_value;
	ZVAL_UNDEF(&property_outputBytes_default_value);
	zend_string *property_outputBytes_name = zend_string_init("outputBytes", sizeof("outputBytes") - 1, 1);
	zend_declare_typed_property(class_entry, property_outputBytes_name, &property_outputBytes_default_value, ZEND_ACC_PUBLIC|ZEND_ACC_READONLY, NULL, (zend_type) ZEND_TYPE_INIT_MASK(MAY_BE_LONG));
	zend_string_release(property_outputBytes_name);

	zval property_outputOverflow_default_value;
	ZVAL_UNDEF(&property_outputOverflow_default_value);
	zend_string *property_outputOverflow_name = zend_string_init("outputOverflow", sizeof("outputOverflow") - 1, 1);
	zend_string *property_outputOverflow_class_DevelopGravity_LuaExt_OverflowBehavior = zend_string_init("DevelopGravity\\LuaExt\\OverflowBehavior", sizeof("DevelopGravity\\LuaExt\\OverflowBehavior")-1, 1);
	zend_declare_typed_property(class_entry, property_outputOverflow_name, &property_outputOverflow_default_value, ZEND_ACC_PUBLIC|ZEND_ACC_READONLY, NULL, (zend_type) ZEND_TYPE_INIT_CLASS(property_outputOverflow_class_DevelopGravity_LuaExt_OverflowBehavior, 0, 0));
	zend_string_release(property_outputOverflow_name);

	zval property_maxLiveCoroutines_default_value;
	ZVAL_UNDEF(&property_maxLiveCoroutines_default_value);
	zend_string *property_maxLiveCoroutines_name = zend_string_init("maxLiveCoroutines", sizeof("maxLiveCoroutines") - 1, 1);
	zend_declare_typed_property(class_entry, property_maxLiveCoroutines_name, &property_maxLiveCoroutines_default_value, ZEND_ACC_PUBLIC|ZEND_ACC_READONLY, NULL, (zend_type) ZEND_TYPE_INIT_MASK(MAY_BE_LONG));
	zend_string_release(property_maxLiveCoroutines_name);

	zval property_maxCoroutineDepth_default_value;
	ZVAL_UNDEF(&property_maxCoroutineDepth_default_value);
	zend_string *property_maxCoroutineDepth_name = zend_string_init("maxCoroutineDepth", sizeof("maxCoroutineDepth") - 1, 1);
	zend_declare_typed_property(class_entry, property_maxCoroutineDepth_name, &property_maxCoroutineDepth_default_value, ZEND_ACC_PUBLIC|ZEND_ACC_READONLY, NULL, (zend_type) ZEND_TYPE_INIT_MASK(MAY_BE_LONG));
	zend_string_release(property_maxCoroutineDepth_name);

	zval property_maxCallDepth_default_value;
	ZVAL_UNDEF(&property_maxCallDepth_default_value);
	zend_string *property_maxCallDepth_name = zend_string_init("maxCallDepth", sizeof("maxCallDepth") - 1, 1);
	zend_declare_typed_property(class_entry, property_maxCallDepth_name, &property_maxCallDepth_default_value, ZEND_ACC_PUBLIC|ZEND_ACC_READONLY, NULL, (zend_type) ZEND_TYPE_INIT_MASK(MAY_BE_LONG));
	zend_string_release(property_maxCallDepth_name);

	zval property_maxModules_default_value;
	ZVAL_UNDEF(&property_maxModules_default_value);
	zend_string *property_maxModules_name = zend_string_init("maxModules", sizeof("maxModules") - 1, 1);
	zend_declare_typed_property(class_entry, property_maxModules_name, &property_maxModules_default_value, ZEND_ACC_PUBLIC|ZEND_ACC_READONLY, NULL, (zend_type) ZEND_TYPE_INIT_MASK(MAY_BE_LONG));
	zend_string_release(property_maxModules_name);

	zval property_maxRequireDepth_default_value;
	ZVAL_UNDEF(&property_maxRequireDepth_default_value);
	zend_string *property_maxRequireDepth_name = zend_string_init("maxRequireDepth", sizeof("maxRequireDepth") - 1, 1);
	zend_declare_typed_property(class_entry, property_maxRequireDepth_name, &property_maxRequireDepth_default_value, ZEND_ACC_PUBLIC|ZEND_ACC_READONLY, NULL, (zend_type) ZEND_TYPE_INIT_MASK(MAY_BE_LONG));
	zend_string_release(property_maxRequireDepth_name);

	zval property_maxStringLength_default_value;
	ZVAL_UNDEF(&property_maxStringLength_default_value);
	zend_string *property_maxStringLength_name = zend_string_init("maxStringLength", sizeof("maxStringLength") - 1, 1);
	zend_declare_typed_property(class_entry, property_maxStringLength_name, &property_maxStringLength_default_value, ZEND_ACC_PUBLIC|ZEND_ACC_READONLY, NULL, (zend_type) ZEND_TYPE_INIT_MASK(MAY_BE_LONG));
	zend_string_release(property_maxStringLength_name);

	zval property_maxSourceBytes_default_value;
	ZVAL_UNDEF(&property_maxSourceBytes_default_value);
	zend_string *property_maxSourceBytes_name = zend_string_init("maxSourceBytes", sizeof("maxSourceBytes") - 1, 1);
	zend_declare_typed_property(class_entry, property_maxSourceBytes_name, &property_maxSourceBytes_default_value, ZEND_ACC_PUBLIC|ZEND_ACC_READONLY, NULL, (zend_type) ZEND_TYPE_INIT_MASK(MAY_BE_LONG));
	zend_string_release(property_maxSourceBytes_name);

	zval property_maxConversionDepth_default_value;
	ZVAL_UNDEF(&property_maxConversionDepth_default_value);
	zend_string *property_maxConversionDepth_name = zend_string_init("maxConversionDepth", sizeof("maxConversionDepth") - 1, 1);
	zend_declare_typed_property(class_entry, property_maxConversionDepth_name, &property_maxConversionDepth_default_value, ZEND_ACC_PUBLIC|ZEND_ACC_READONLY, NULL, (zend_type) ZEND_TYPE_INIT_MASK(MAY_BE_LONG));
	zend_string_release(property_maxConversionDepth_name);

	zval property_maxCachedChunks_default_value;
	ZVAL_UNDEF(&property_maxCachedChunks_default_value);
	zend_string *property_maxCachedChunks_name = zend_string_init("maxCachedChunks", sizeof("maxCachedChunks") - 1, 1);
	zend_declare_typed_property(class_entry, property_maxCachedChunks_name, &property_maxCachedChunks_default_value, ZEND_ACC_PUBLIC|ZEND_ACC_READONLY, NULL, (zend_type) ZEND_TYPE_INIT_MASK(MAY_BE_LONG));
	zend_string_release(property_maxCachedChunks_name);

	return class_entry;
}

static zend_class_entry *register_class_DevelopGravity_LuaExt_VfsQuota(void)
{
	zend_class_entry ce, *class_entry;

	INIT_NS_CLASS_ENTRY(ce, "DevelopGravity\\LuaExt", "VfsQuota", class_DevelopGravity_LuaExt_VfsQuota_methods);
	class_entry = zend_register_internal_class_with_flags(&ce, NULL, ZEND_ACC_FINAL|ZEND_ACC_NO_DYNAMIC_PROPERTIES|ZEND_ACC_READONLY_CLASS);

	zval property_maxOpenHandles_default_value;
	ZVAL_UNDEF(&property_maxOpenHandles_default_value);
	zend_string *property_maxOpenHandles_name = zend_string_init("maxOpenHandles", sizeof("maxOpenHandles") - 1, 1);
	zend_declare_typed_property(class_entry, property_maxOpenHandles_name, &property_maxOpenHandles_default_value, ZEND_ACC_PUBLIC|ZEND_ACC_READONLY, NULL, (zend_type) ZEND_TYPE_INIT_MASK(MAY_BE_LONG));
	zend_string_release(property_maxOpenHandles_name);

	zval property_maxFileBytes_default_value;
	ZVAL_UNDEF(&property_maxFileBytes_default_value);
	zend_string *property_maxFileBytes_name = zend_string_init("maxFileBytes", sizeof("maxFileBytes") - 1, 1);
	zend_declare_typed_property(class_entry, property_maxFileBytes_name, &property_maxFileBytes_default_value, ZEND_ACC_PUBLIC|ZEND_ACC_READONLY, NULL, (zend_type) ZEND_TYPE_INIT_MASK(MAY_BE_LONG));
	zend_string_release(property_maxFileBytes_name);

	zval property_maxTotalBytes_default_value;
	ZVAL_UNDEF(&property_maxTotalBytes_default_value);
	zend_string *property_maxTotalBytes_name = zend_string_init("maxTotalBytes", sizeof("maxTotalBytes") - 1, 1);
	zend_declare_typed_property(class_entry, property_maxTotalBytes_name, &property_maxTotalBytes_default_value, ZEND_ACC_PUBLIC|ZEND_ACC_READONLY, NULL, (zend_type) ZEND_TYPE_INIT_MASK(MAY_BE_LONG));
	zend_string_release(property_maxTotalBytes_name);

	zval property_maxFiles_default_value;
	ZVAL_UNDEF(&property_maxFiles_default_value);
	zend_string *property_maxFiles_name = zend_string_init("maxFiles", sizeof("maxFiles") - 1, 1);
	zend_declare_typed_property(class_entry, property_maxFiles_name, &property_maxFiles_default_value, ZEND_ACC_PUBLIC|ZEND_ACC_READONLY, NULL, (zend_type) ZEND_TYPE_INIT_MASK(MAY_BE_LONG));
	zend_string_release(property_maxFiles_name);

	zval property_maxOperations_default_value;
	ZVAL_UNDEF(&property_maxOperations_default_value);
	zend_string *property_maxOperations_name = zend_string_init("maxOperations", sizeof("maxOperations") - 1, 1);
	zend_declare_typed_property(class_entry, property_maxOperations_name, &property_maxOperations_default_value, ZEND_ACC_PUBLIC|ZEND_ACC_READONLY, NULL, (zend_type) ZEND_TYPE_INIT_MASK(MAY_BE_LONG));
	zend_string_release(property_maxOperations_name);

	zval property_maxPathLength_default_value;
	ZVAL_UNDEF(&property_maxPathLength_default_value);
	zend_string *property_maxPathLength_name = zend_string_init("maxPathLength", sizeof("maxPathLength") - 1, 1);
	zend_declare_typed_property(class_entry, property_maxPathLength_name, &property_maxPathLength_default_value, ZEND_ACC_PUBLIC|ZEND_ACC_READONLY, NULL, (zend_type) ZEND_TYPE_INIT_MASK(MAY_BE_LONG));
	zend_string_release(property_maxPathLength_name);

	zval property_maxPathDepth_default_value;
	ZVAL_UNDEF(&property_maxPathDepth_default_value);
	zend_string *property_maxPathDepth_name = zend_string_init("maxPathDepth", sizeof("maxPathDepth") - 1, 1);
	zend_declare_typed_property(class_entry, property_maxPathDepth_name, &property_maxPathDepth_default_value, ZEND_ACC_PUBLIC|ZEND_ACC_READONLY, NULL, (zend_type) ZEND_TYPE_INIT_MASK(MAY_BE_LONG));
	zend_string_release(property_maxPathDepth_name);

	zval property_billWallTime_default_value;
	ZVAL_UNDEF(&property_billWallTime_default_value);
	zend_string *property_billWallTime_name = zend_string_init("billWallTime", sizeof("billWallTime") - 1, 1);
	zend_declare_typed_property(class_entry, property_billWallTime_name, &property_billWallTime_default_value, ZEND_ACC_PUBLIC|ZEND_ACC_READONLY, NULL, (zend_type) ZEND_TYPE_INIT_MASK(MAY_BE_BOOL));
	zend_string_release(property_billWallTime_name);

	return class_entry;
}

static zend_class_entry *register_class_DevelopGravity_LuaExt_SandboxConfig(void)
{
	zend_class_entry ce, *class_entry;

	INIT_NS_CLASS_ENTRY(ce, "DevelopGravity\\LuaExt", "SandboxConfig", class_DevelopGravity_LuaExt_SandboxConfig_methods);
	class_entry = zend_register_internal_class_with_flags(&ce, NULL, ZEND_ACC_FINAL|ZEND_ACC_NO_DYNAMIC_PROPERTIES|ZEND_ACC_READONLY_CLASS);

	zval property_capabilities_default_value;
	ZVAL_UNDEF(&property_capabilities_default_value);
	zend_string *property_capabilities_name = zend_string_init("capabilities", sizeof("capabilities") - 1, 1);
	zend_string *property_capabilities_class_DevelopGravity_LuaExt_Capabilities = zend_string_init("DevelopGravity\\LuaExt\\Capabilities", sizeof("DevelopGravity\\LuaExt\\Capabilities")-1, 1);
	zend_declare_typed_property(class_entry, property_capabilities_name, &property_capabilities_default_value, ZEND_ACC_PUBLIC|ZEND_ACC_READONLY, NULL, (zend_type) ZEND_TYPE_INIT_CLASS(property_capabilities_class_DevelopGravity_LuaExt_Capabilities, 0, MAY_BE_NULL));
	zend_string_release(property_capabilities_name);

	zval property_limits_default_value;
	ZVAL_UNDEF(&property_limits_default_value);
	zend_string *property_limits_name = zend_string_init("limits", sizeof("limits") - 1, 1);
	zend_string *property_limits_class_DevelopGravity_LuaExt_Limits = zend_string_init("DevelopGravity\\LuaExt\\Limits", sizeof("DevelopGravity\\LuaExt\\Limits")-1, 1);
	zend_declare_typed_property(class_entry, property_limits_name, &property_limits_default_value, ZEND_ACC_PUBLIC|ZEND_ACC_READONLY, NULL, (zend_type) ZEND_TYPE_INIT_CLASS(property_limits_class_DevelopGravity_LuaExt_Limits, 0, MAY_BE_NULL));
	zend_string_release(property_limits_name);

	zval property_filesystem_default_value;
	ZVAL_UNDEF(&property_filesystem_default_value);
	zend_string *property_filesystem_name = zend_string_init("filesystem", sizeof("filesystem") - 1, 1);
	zend_string *property_filesystem_class_DevelopGravity_LuaExt_FileSystem = zend_string_init("DevelopGravity\\LuaExt\\FileSystem", sizeof("DevelopGravity\\LuaExt\\FileSystem")-1, 1);
	zend_declare_typed_property(class_entry, property_filesystem_name, &property_filesystem_default_value, ZEND_ACC_PUBLIC|ZEND_ACC_READONLY, NULL, (zend_type) ZEND_TYPE_INIT_CLASS(property_filesystem_class_DevelopGravity_LuaExt_FileSystem, 0, MAY_BE_NULL));
	zend_string_release(property_filesystem_name);

	zval property_vfsQuota_default_value;
	ZVAL_UNDEF(&property_vfsQuota_default_value);
	zend_string *property_vfsQuota_name = zend_string_init("vfsQuota", sizeof("vfsQuota") - 1, 1);
	zend_string *property_vfsQuota_class_DevelopGravity_LuaExt_VfsQuota = zend_string_init("DevelopGravity\\LuaExt\\VfsQuota", sizeof("DevelopGravity\\LuaExt\\VfsQuota")-1, 1);
	zend_declare_typed_property(class_entry, property_vfsQuota_name, &property_vfsQuota_default_value, ZEND_ACC_PUBLIC|ZEND_ACC_READONLY, NULL, (zend_type) ZEND_TYPE_INIT_CLASS(property_vfsQuota_class_DevelopGravity_LuaExt_VfsQuota, 0, MAY_BE_NULL));
	zend_string_release(property_vfsQuota_name);

	zval property_moduleResolver_default_value;
	ZVAL_UNDEF(&property_moduleResolver_default_value);
	zend_string *property_moduleResolver_name = zend_string_init("moduleResolver", sizeof("moduleResolver") - 1, 1);
	zend_string *property_moduleResolver_class_DevelopGravity_LuaExt_ModuleResolver = zend_string_init("DevelopGravity\\LuaExt\\ModuleResolver", sizeof("DevelopGravity\\LuaExt\\ModuleResolver")-1, 1);
	zend_declare_typed_property(class_entry, property_moduleResolver_name, &property_moduleResolver_default_value, ZEND_ACC_PUBLIC|ZEND_ACC_READONLY, NULL, (zend_type) ZEND_TYPE_INIT_CLASS(property_moduleResolver_class_DevelopGravity_LuaExt_ModuleResolver, 0, MAY_BE_NULL));
	zend_string_release(property_moduleResolver_name);

	zval property_modulePaths_default_value;
	ZVAL_UNDEF(&property_modulePaths_default_value);
	zend_string *property_modulePaths_name = zend_string_init("modulePaths", sizeof("modulePaths") - 1, 1);
	zend_declare_typed_property(class_entry, property_modulePaths_name, &property_modulePaths_default_value, ZEND_ACC_PUBLIC|ZEND_ACC_READONLY, NULL, (zend_type) ZEND_TYPE_INIT_MASK(MAY_BE_ARRAY));
	zend_string_release(property_modulePaths_name);

	zval property_outputMode_default_value;
	ZVAL_UNDEF(&property_outputMode_default_value);
	zend_string *property_outputMode_name = zend_string_init("outputMode", sizeof("outputMode") - 1, 1);
	zend_string *property_outputMode_class_DevelopGravity_LuaExt_OutputMode = zend_string_init("DevelopGravity\\LuaExt\\OutputMode", sizeof("DevelopGravity\\LuaExt\\OutputMode")-1, 1);
	zend_declare_typed_property(class_entry, property_outputMode_name, &property_outputMode_default_value, ZEND_ACC_PUBLIC|ZEND_ACC_READONLY, NULL, (zend_type) ZEND_TYPE_INIT_CLASS(property_outputMode_class_DevelopGravity_LuaExt_OutputMode, 0, 0));
	zend_string_release(property_outputMode_name);

	zval property_outputCallback_default_value;
	ZVAL_UNDEF(&property_outputCallback_default_value);
	zend_string *property_outputCallback_name = zend_string_init("outputCallback", sizeof("outputCallback") - 1, 1);
	zend_string *property_outputCallback_class_Closure = zend_string_init("Closure", sizeof("Closure")-1, 1);
	zend_declare_typed_property(class_entry, property_outputCallback_name, &property_outputCallback_default_value, ZEND_ACC_PUBLIC|ZEND_ACC_READONLY, NULL, (zend_type) ZEND_TYPE_INIT_CLASS(property_outputCallback_class_Closure, 0, MAY_BE_NULL));
	zend_string_release(property_outputCallback_name);

	zval property_outputChunkBytes_default_value;
	ZVAL_UNDEF(&property_outputChunkBytes_default_value);
	zend_string *property_outputChunkBytes_name = zend_string_init("outputChunkBytes", sizeof("outputChunkBytes") - 1, 1);
	zend_declare_typed_property(class_entry, property_outputChunkBytes_name, &property_outputChunkBytes_default_value, ZEND_ACC_PUBLIC|ZEND_ACC_READONLY, NULL, (zend_type) ZEND_TYPE_INIT_MASK(MAY_BE_LONG));
	zend_string_release(property_outputChunkBytes_name);

	zval property_seed_default_value;
	ZVAL_UNDEF(&property_seed_default_value);
	zend_string *property_seed_name = zend_string_init("seed", sizeof("seed") - 1, 1);
	zend_declare_typed_property(class_entry, property_seed_name, &property_seed_default_value, ZEND_ACC_PUBLIC|ZEND_ACC_READONLY, NULL, (zend_type) ZEND_TYPE_INIT_MASK(MAY_BE_LONG|MAY_BE_NULL));
	zend_string_release(property_seed_name);

	zval property_deterministic_default_value;
	ZVAL_UNDEF(&property_deterministic_default_value);
	zend_string *property_deterministic_name = zend_string_init("deterministic", sizeof("deterministic") - 1, 1);
	zend_declare_typed_property(class_entry, property_deterministic_name, &property_deterministic_default_value, ZEND_ACC_PUBLIC|ZEND_ACC_READONLY, NULL, (zend_type) ZEND_TYPE_INIT_MASK(MAY_BE_BOOL));
	zend_string_release(property_deterministic_name);

	zval property_cacheCompiledChunks_default_value;
	ZVAL_UNDEF(&property_cacheCompiledChunks_default_value);
	zend_string *property_cacheCompiledChunks_name = zend_string_init("cacheCompiledChunks", sizeof("cacheCompiledChunks") - 1, 1);
	zend_declare_typed_property(class_entry, property_cacheCompiledChunks_name, &property_cacheCompiledChunks_default_value, ZEND_ACC_PUBLIC|ZEND_ACC_READONLY, NULL, (zend_type) ZEND_TYPE_INIT_MASK(MAY_BE_BOOL));
	zend_string_release(property_cacheCompiledChunks_name);

	zval property_sealMode_default_value;
	ZVAL_UNDEF(&property_sealMode_default_value);
	zend_string *property_sealMode_name = zend_string_init("sealMode", sizeof("sealMode") - 1, 1);
	zend_string *property_sealMode_class_DevelopGravity_LuaExt_SealMode = zend_string_init("DevelopGravity\\LuaExt\\SealMode", sizeof("DevelopGravity\\LuaExt\\SealMode")-1, 1);
	zend_declare_typed_property(class_entry, property_sealMode_name, &property_sealMode_default_value, ZEND_ACC_PUBLIC|ZEND_ACC_READONLY, NULL, (zend_type) ZEND_TYPE_INIT_CLASS(property_sealMode_class_DevelopGravity_LuaExt_SealMode, 0, 0));
	zend_string_release(property_sealMode_name);

	zval property_bytecodeKey_default_value;
	ZVAL_UNDEF(&property_bytecodeKey_default_value);
	zend_string *property_bytecodeKey_name = zend_string_init("bytecodeKey", sizeof("bytecodeKey") - 1, 1);
	zend_declare_typed_property(class_entry, property_bytecodeKey_name, &property_bytecodeKey_default_value, ZEND_ACC_PUBLIC|ZEND_ACC_READONLY, NULL, (zend_type) ZEND_TYPE_INIT_MASK(MAY_BE_STRING|MAY_BE_NULL));
	zend_string_release(property_bytecodeKey_name);

	return class_entry;
}

static zend_class_entry *register_class_DevelopGravity_LuaExt_SandboxStats(zend_class_entry *class_entry_JsonSerializable)
{
	zend_class_entry ce, *class_entry;

	INIT_NS_CLASS_ENTRY(ce, "DevelopGravity\\LuaExt", "SandboxStats", class_DevelopGravity_LuaExt_SandboxStats_methods);
	class_entry = zend_register_internal_class_with_flags(&ce, NULL, ZEND_ACC_FINAL|ZEND_ACC_NO_DYNAMIC_PROPERTIES|ZEND_ACC_READONLY_CLASS);
	zend_class_implements(class_entry, 1, class_entry_JsonSerializable);

	zval property_memoryBytes_default_value;
	ZVAL_UNDEF(&property_memoryBytes_default_value);
	zend_string *property_memoryBytes_name = zend_string_init("memoryBytes", sizeof("memoryBytes") - 1, 1);
	zend_declare_typed_property(class_entry, property_memoryBytes_name, &property_memoryBytes_default_value, ZEND_ACC_PUBLIC|ZEND_ACC_READONLY, NULL, (zend_type) ZEND_TYPE_INIT_MASK(MAY_BE_LONG));
	zend_string_release(property_memoryBytes_name);

	zval property_peakMemoryBytes_default_value;
	ZVAL_UNDEF(&property_peakMemoryBytes_default_value);
	zend_string *property_peakMemoryBytes_name = zend_string_init("peakMemoryBytes", sizeof("peakMemoryBytes") - 1, 1);
	zend_declare_typed_property(class_entry, property_peakMemoryBytes_name, &property_peakMemoryBytes_default_value, ZEND_ACC_PUBLIC|ZEND_ACC_READONLY, NULL, (zend_type) ZEND_TYPE_INIT_MASK(MAY_BE_LONG));
	zend_string_release(property_peakMemoryBytes_name);

	zval property_memoryLimitBytes_default_value;
	ZVAL_UNDEF(&property_memoryLimitBytes_default_value);
	zend_string *property_memoryLimitBytes_name = zend_string_init("memoryLimitBytes", sizeof("memoryLimitBytes") - 1, 1);
	zend_declare_typed_property(class_entry, property_memoryLimitBytes_name, &property_memoryLimitBytes_default_value, ZEND_ACC_PUBLIC|ZEND_ACC_READONLY, NULL, (zend_type) ZEND_TYPE_INIT_MASK(MAY_BE_LONG));
	zend_string_release(property_memoryLimitBytes_name);

	zval property_cpuSeconds_default_value;
	ZVAL_UNDEF(&property_cpuSeconds_default_value);
	zend_string *property_cpuSeconds_name = zend_string_init("cpuSeconds", sizeof("cpuSeconds") - 1, 1);
	zend_declare_typed_property(class_entry, property_cpuSeconds_name, &property_cpuSeconds_default_value, ZEND_ACC_PUBLIC|ZEND_ACC_READONLY, NULL, (zend_type) ZEND_TYPE_INIT_MASK(MAY_BE_DOUBLE));
	zend_string_release(property_cpuSeconds_name);

	zval property_wallClockSeconds_default_value;
	ZVAL_UNDEF(&property_wallClockSeconds_default_value);
	zend_string *property_wallClockSeconds_name = zend_string_init("wallClockSeconds", sizeof("wallClockSeconds") - 1, 1);
	zend_declare_typed_property(class_entry, property_wallClockSeconds_name, &property_wallClockSeconds_default_value, ZEND_ACC_PUBLIC|ZEND_ACC_READONLY, NULL, (zend_type) ZEND_TYPE_INIT_MASK(MAY_BE_DOUBLE));
	zend_string_release(property_wallClockSeconds_name);

	zval property_outputBytes_default_value;
	ZVAL_UNDEF(&property_outputBytes_default_value);
	zend_string *property_outputBytes_name = zend_string_init("outputBytes", sizeof("outputBytes") - 1, 1);
	zend_declare_typed_property(class_entry, property_outputBytes_name, &property_outputBytes_default_value, ZEND_ACC_PUBLIC|ZEND_ACC_READONLY, NULL, (zend_type) ZEND_TYPE_INIT_MASK(MAY_BE_LONG));
	zend_string_release(property_outputBytes_name);

	zval property_outputTruncated_default_value;
	ZVAL_UNDEF(&property_outputTruncated_default_value);
	zend_string *property_outputTruncated_name = zend_string_init("outputTruncated", sizeof("outputTruncated") - 1, 1);
	zend_declare_typed_property(class_entry, property_outputTruncated_name, &property_outputTruncated_default_value, ZEND_ACC_PUBLIC|ZEND_ACC_READONLY, NULL, (zend_type) ZEND_TYPE_INIT_MASK(MAY_BE_BOOL));
	zend_string_release(property_outputTruncated_name);

	zval property_liveCoroutines_default_value;
	ZVAL_UNDEF(&property_liveCoroutines_default_value);
	zend_string *property_liveCoroutines_name = zend_string_init("liveCoroutines", sizeof("liveCoroutines") - 1, 1);
	zend_declare_typed_property(class_entry, property_liveCoroutines_name, &property_liveCoroutines_default_value, ZEND_ACC_PUBLIC|ZEND_ACC_READONLY, NULL, (zend_type) ZEND_TYPE_INIT_MASK(MAY_BE_LONG));
	zend_string_release(property_liveCoroutines_name);

	zval property_peakCoroutineDepth_default_value;
	ZVAL_UNDEF(&property_peakCoroutineDepth_default_value);
	zend_string *property_peakCoroutineDepth_name = zend_string_init("peakCoroutineDepth", sizeof("peakCoroutineDepth") - 1, 1);
	zend_declare_typed_property(class_entry, property_peakCoroutineDepth_name, &property_peakCoroutineDepth_default_value, ZEND_ACC_PUBLIC|ZEND_ACC_READONLY, NULL, (zend_type) ZEND_TYPE_INIT_MASK(MAY_BE_LONG));
	zend_string_release(property_peakCoroutineDepth_name);

	zval property_modulesLoaded_default_value;
	ZVAL_UNDEF(&property_modulesLoaded_default_value);
	zend_string *property_modulesLoaded_name = zend_string_init("modulesLoaded", sizeof("modulesLoaded") - 1, 1);
	zend_declare_typed_property(class_entry, property_modulesLoaded_name, &property_modulesLoaded_default_value, ZEND_ACC_PUBLIC|ZEND_ACC_READONLY, NULL, (zend_type) ZEND_TYPE_INIT_MASK(MAY_BE_LONG));
	zend_string_release(property_modulesLoaded_name);

	zval property_cachedChunks_default_value;
	ZVAL_UNDEF(&property_cachedChunks_default_value);
	zend_string *property_cachedChunks_name = zend_string_init("cachedChunks", sizeof("cachedChunks") - 1, 1);
	zend_declare_typed_property(class_entry, property_cachedChunks_name, &property_cachedChunks_default_value, ZEND_ACC_PUBLIC|ZEND_ACC_READONLY, NULL, (zend_type) ZEND_TYPE_INIT_MASK(MAY_BE_LONG));
	zend_string_release(property_cachedChunks_name);

	zval property_vfsOperations_default_value;
	ZVAL_UNDEF(&property_vfsOperations_default_value);
	zend_string *property_vfsOperations_name = zend_string_init("vfsOperations", sizeof("vfsOperations") - 1, 1);
	zend_declare_typed_property(class_entry, property_vfsOperations_name, &property_vfsOperations_default_value, ZEND_ACC_PUBLIC|ZEND_ACC_READONLY, NULL, (zend_type) ZEND_TYPE_INIT_MASK(MAY_BE_LONG));
	zend_string_release(property_vfsOperations_name);

	zval property_vfsBytes_default_value;
	ZVAL_UNDEF(&property_vfsBytes_default_value);
	zend_string *property_vfsBytes_name = zend_string_init("vfsBytes", sizeof("vfsBytes") - 1, 1);
	zend_declare_typed_property(class_entry, property_vfsBytes_name, &property_vfsBytes_default_value, ZEND_ACC_PUBLIC|ZEND_ACC_READONLY, NULL, (zend_type) ZEND_TYPE_INIT_MASK(MAY_BE_LONG));
	zend_string_release(property_vfsBytes_name);

	zval property_gcCollections_default_value;
	ZVAL_UNDEF(&property_gcCollections_default_value);
	zend_string *property_gcCollections_name = zend_string_init("gcCollections", sizeof("gcCollections") - 1, 1);
	zend_declare_typed_property(class_entry, property_gcCollections_name, &property_gcCollections_default_value, ZEND_ACC_PUBLIC|ZEND_ACC_READONLY, NULL, (zend_type) ZEND_TYPE_INIT_MASK(MAY_BE_LONG));
	zend_string_release(property_gcCollections_name);

	zval property_luaCallsIn_default_value;
	ZVAL_UNDEF(&property_luaCallsIn_default_value);
	zend_string *property_luaCallsIn_name = zend_string_init("luaCallsIn", sizeof("luaCallsIn") - 1, 1);
	zend_declare_typed_property(class_entry, property_luaCallsIn_name, &property_luaCallsIn_default_value, ZEND_ACC_PUBLIC|ZEND_ACC_READONLY, NULL, (zend_type) ZEND_TYPE_INIT_MASK(MAY_BE_LONG));
	zend_string_release(property_luaCallsIn_name);

	zval property_phpCallsOut_default_value;
	ZVAL_UNDEF(&property_phpCallsOut_default_value);
	zend_string *property_phpCallsOut_name = zend_string_init("phpCallsOut", sizeof("phpCallsOut") - 1, 1);
	zend_declare_typed_property(class_entry, property_phpCallsOut_name, &property_phpCallsOut_default_value, ZEND_ACC_PUBLIC|ZEND_ACC_READONLY, NULL, (zend_type) ZEND_TYPE_INIT_MASK(MAY_BE_LONG));
	zend_string_release(property_phpCallsOut_name);

	return class_entry;
}

static zend_class_entry *register_class_DevelopGravity_LuaExt_ValidationResult(zend_class_entry *class_entry_JsonSerializable)
{
	zend_class_entry ce, *class_entry;

	INIT_NS_CLASS_ENTRY(ce, "DevelopGravity\\LuaExt", "ValidationResult", class_DevelopGravity_LuaExt_ValidationResult_methods);
	class_entry = zend_register_internal_class_with_flags(&ce, NULL, ZEND_ACC_FINAL|ZEND_ACC_NO_DYNAMIC_PROPERTIES|ZEND_ACC_READONLY_CLASS);
	zend_class_implements(class_entry, 1, class_entry_JsonSerializable);

	zval property_valid_default_value;
	ZVAL_UNDEF(&property_valid_default_value);
	zend_string *property_valid_name = zend_string_init("valid", sizeof("valid") - 1, 1);
	zend_declare_typed_property(class_entry, property_valid_name, &property_valid_default_value, ZEND_ACC_PUBLIC|ZEND_ACC_READONLY, NULL, (zend_type) ZEND_TYPE_INIT_MASK(MAY_BE_BOOL));
	zend_string_release(property_valid_name);

	zval property_message_default_value;
	ZVAL_UNDEF(&property_message_default_value);
	zend_declare_typed_property(class_entry, ZSTR_KNOWN(ZEND_STR_MESSAGE), &property_message_default_value, ZEND_ACC_PUBLIC|ZEND_ACC_READONLY, NULL, (zend_type) ZEND_TYPE_INIT_MASK(MAY_BE_STRING|MAY_BE_NULL));

	zval property_line_default_value;
	ZVAL_UNDEF(&property_line_default_value);
	zend_declare_typed_property(class_entry, ZSTR_KNOWN(ZEND_STR_LINE), &property_line_default_value, ZEND_ACC_PUBLIC|ZEND_ACC_READONLY, NULL, (zend_type) ZEND_TYPE_INIT_MASK(MAY_BE_LONG|MAY_BE_NULL));

	zval property_chunkName_default_value;
	ZVAL_UNDEF(&property_chunkName_default_value);
	zend_string *property_chunkName_name = zend_string_init("chunkName", sizeof("chunkName") - 1, 1);
	zend_declare_typed_property(class_entry, property_chunkName_name, &property_chunkName_default_value, ZEND_ACC_PUBLIC|ZEND_ACC_READONLY, NULL, (zend_type) ZEND_TYPE_INIT_MASK(MAY_BE_STRING|MAY_BE_NULL));
	zend_string_release(property_chunkName_name);

	return class_entry;
}

static zend_class_entry *register_class_DevelopGravity_LuaExt_Sandbox(void)
{
	zend_class_entry ce, *class_entry;

	INIT_NS_CLASS_ENTRY(ce, "DevelopGravity\\LuaExt", "Sandbox", class_DevelopGravity_LuaExt_Sandbox_methods);
	class_entry = zend_register_internal_class_with_flags(&ce, NULL, ZEND_ACC_FINAL|ZEND_ACC_NO_DYNAMIC_PROPERTIES|ZEND_ACC_NOT_SERIALIZABLE);


	zend_string *attribute_name_NoDiscard_func_validate_0 = zend_string_init_interned("NoDiscard", sizeof("NoDiscard") - 1, 1);
	zend_add_function_attribute(zend_hash_str_find_ptr(&class_entry->function_table, "validate", sizeof("validate") - 1), attribute_name_NoDiscard_func_validate_0, 0);
	zend_string_release(attribute_name_NoDiscard_func_validate_0);

	zend_string *attribute_name_NoDiscard_func_eval_0 = zend_string_init_interned("NoDiscard", sizeof("NoDiscard") - 1, 1);
	zend_add_function_attribute(zend_hash_str_find_ptr(&class_entry->function_table, "eval", sizeof("eval") - 1), attribute_name_NoDiscard_func_eval_0, 0);
	zend_string_release(attribute_name_NoDiscard_func_eval_0);

	zend_string *attribute_name_NoDiscard_func_call_0 = zend_string_init_interned("NoDiscard", sizeof("NoDiscard") - 1, 1);
	zend_add_function_attribute(zend_hash_str_find_ptr(&class_entry->function_table, "call", sizeof("call") - 1), attribute_name_NoDiscard_func_call_0, 0);
	zend_string_release(attribute_name_NoDiscard_func_call_0);

	zend_string *attribute_name_NoDiscard_func_takeoutput_0 = zend_string_init_interned("NoDiscard", sizeof("NoDiscard") - 1, 1);
	zend_add_function_attribute(zend_hash_str_find_ptr(&class_entry->function_table, "takeoutput", sizeof("takeoutput") - 1), attribute_name_NoDiscard_func_takeoutput_0, 0);
	zend_string_release(attribute_name_NoDiscard_func_takeoutput_0);

	return class_entry;
}

static zend_class_entry *register_class_DevelopGravity_LuaExt_LuaFunction(void)
{
	zend_class_entry ce, *class_entry;

	INIT_NS_CLASS_ENTRY(ce, "DevelopGravity\\LuaExt", "LuaFunction", class_DevelopGravity_LuaExt_LuaFunction_methods);
	class_entry = zend_register_internal_class_with_flags(&ce, NULL, ZEND_ACC_FINAL|ZEND_ACC_NO_DYNAMIC_PROPERTIES|ZEND_ACC_NOT_SERIALIZABLE);


	zend_string *attribute_name_NoDiscard_func_call_0 = zend_string_init_interned("NoDiscard", sizeof("NoDiscard") - 1, 1);
	zend_add_function_attribute(zend_hash_str_find_ptr(&class_entry->function_table, "call", sizeof("call") - 1), attribute_name_NoDiscard_func_call_0, 0);
	zend_string_release(attribute_name_NoDiscard_func_call_0);

	zend_string *attribute_name_NoDiscard_func___invoke_0 = zend_string_init_interned("NoDiscard", sizeof("NoDiscard") - 1, 1);
	zend_add_function_attribute(zend_hash_str_find_ptr(&class_entry->function_table, "__invoke", sizeof("__invoke") - 1), attribute_name_NoDiscard_func___invoke_0, 0);
	zend_string_release(attribute_name_NoDiscard_func___invoke_0);

	return class_entry;
}

static zend_class_entry *register_class_DevelopGravity_LuaExt_FileStat(void)
{
	zend_class_entry ce, *class_entry;

	INIT_NS_CLASS_ENTRY(ce, "DevelopGravity\\LuaExt", "FileStat", class_DevelopGravity_LuaExt_FileStat_methods);
	class_entry = zend_register_internal_class_with_flags(&ce, NULL, ZEND_ACC_FINAL|ZEND_ACC_NO_DYNAMIC_PROPERTIES|ZEND_ACC_READONLY_CLASS);

	zval property_size_default_value;
	ZVAL_UNDEF(&property_size_default_value);
	zend_string *property_size_name = zend_string_init("size", sizeof("size") - 1, 1);
	zend_declare_typed_property(class_entry, property_size_name, &property_size_default_value, ZEND_ACC_PUBLIC|ZEND_ACC_READONLY, NULL, (zend_type) ZEND_TYPE_INIT_MASK(MAY_BE_LONG));
	zend_string_release(property_size_name);

	zval property_mtime_default_value;
	ZVAL_UNDEF(&property_mtime_default_value);
	zend_string *property_mtime_name = zend_string_init("mtime", sizeof("mtime") - 1, 1);
	zend_declare_typed_property(class_entry, property_mtime_name, &property_mtime_default_value, ZEND_ACC_PUBLIC|ZEND_ACC_READONLY, NULL, (zend_type) ZEND_TYPE_INIT_MASK(MAY_BE_LONG));
	zend_string_release(property_mtime_name);

	zval property_isDirectory_default_value;
	ZVAL_UNDEF(&property_isDirectory_default_value);
	zend_string *property_isDirectory_name = zend_string_init("isDirectory", sizeof("isDirectory") - 1, 1);
	zend_declare_typed_property(class_entry, property_isDirectory_name, &property_isDirectory_default_value, ZEND_ACC_PUBLIC|ZEND_ACC_READONLY, NULL, (zend_type) ZEND_TYPE_INIT_MASK(MAY_BE_BOOL));
	zend_string_release(property_isDirectory_name);

	return class_entry;
}

static zend_class_entry *register_class_DevelopGravity_LuaExt_FileSystem(void)
{
	zend_class_entry ce, *class_entry;

	INIT_NS_CLASS_ENTRY(ce, "DevelopGravity\\LuaExt", "FileSystem", class_DevelopGravity_LuaExt_FileSystem_methods);
	class_entry = zend_register_internal_interface(&ce);

	return class_entry;
}

static zend_class_entry *register_class_DevelopGravity_LuaExt_RangedFileSystem(zend_class_entry *class_entry_DevelopGravity_LuaExt_FileSystem)
{
	zend_class_entry ce, *class_entry;

	INIT_NS_CLASS_ENTRY(ce, "DevelopGravity\\LuaExt", "RangedFileSystem", class_DevelopGravity_LuaExt_RangedFileSystem_methods);
	class_entry = zend_register_internal_interface(&ce);
	zend_class_implements(class_entry, 1, class_entry_DevelopGravity_LuaExt_FileSystem);

	return class_entry;
}

static zend_class_entry *register_class_DevelopGravity_LuaExt_ModuleSource(void)
{
	zend_class_entry ce, *class_entry;

	INIT_NS_CLASS_ENTRY(ce, "DevelopGravity\\LuaExt", "ModuleSource", class_DevelopGravity_LuaExt_ModuleSource_methods);
	class_entry = zend_register_internal_class_with_flags(&ce, NULL, ZEND_ACC_FINAL|ZEND_ACC_NO_DYNAMIC_PROPERTIES|ZEND_ACC_READONLY_CLASS);

	zval property_code_default_value;
	ZVAL_UNDEF(&property_code_default_value);
	zend_declare_typed_property(class_entry, ZSTR_KNOWN(ZEND_STR_CODE), &property_code_default_value, ZEND_ACC_PUBLIC|ZEND_ACC_READONLY, NULL, (zend_type) ZEND_TYPE_INIT_MASK(MAY_BE_STRING));

	zval property_chunkName_default_value;
	ZVAL_UNDEF(&property_chunkName_default_value);
	zend_string *property_chunkName_name = zend_string_init("chunkName", sizeof("chunkName") - 1, 1);
	zend_declare_typed_property(class_entry, property_chunkName_name, &property_chunkName_default_value, ZEND_ACC_PUBLIC|ZEND_ACC_READONLY, NULL, (zend_type) ZEND_TYPE_INIT_MASK(MAY_BE_STRING));
	zend_string_release(property_chunkName_name);

	zval property_isBytecode_default_value;
	ZVAL_UNDEF(&property_isBytecode_default_value);
	zend_string *property_isBytecode_name = zend_string_init("isBytecode", sizeof("isBytecode") - 1, 1);
	zend_declare_typed_property(class_entry, property_isBytecode_name, &property_isBytecode_default_value, ZEND_ACC_PUBLIC|ZEND_ACC_READONLY, NULL, (zend_type) ZEND_TYPE_INIT_MASK(MAY_BE_BOOL));
	zend_string_release(property_isBytecode_name);

	return class_entry;
}

static zend_class_entry *register_class_DevelopGravity_LuaExt_ModuleResolver(void)
{
	zend_class_entry ce, *class_entry;

	INIT_NS_CLASS_ENTRY(ce, "DevelopGravity\\LuaExt", "ModuleResolver", class_DevelopGravity_LuaExt_ModuleResolver_methods);
	class_entry = zend_register_internal_interface(&ce);

	return class_entry;
}
