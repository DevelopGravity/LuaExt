/*
 * luaext — methods whose subsystem has not been written yet.
 *
 * Every class declared under stubs/ is registered from MINIT, and the generated
 * arginfo references one C function per declared method, so all of them have to
 * exist for the module to link. Rather than scattering half-written versions
 * across the files that will eventually own them, the not-yet-implemented ones
 * are collected here and refuse plainly when called.
 *
 * This file shrinks to nothing as the subsystems land: an implementer moves the
 * method into its real home (config objects, the LuaFunction handle, the error
 * machinery) and deletes the entry here. Sandbox's own pending methods stay in
 * luaext_sandbox.c, where they already belong.
 */

#include "luaext_types.h"

#include <Zend/zend_exceptions.h>

/* Refuse a call to a method whose subsystem does not exist yet. */
#define LUAEXT_PENDING(qualified_name)                                                             \
	do {                                                                                           \
		zend_throw_error(NULL, "%s() is not implemented yet", (qualified_name));                   \
		RETURN_THROWS();                                                                           \
	} while (0)

/* -------------------------------------------------------------------------
 * Configuration value objects
 *
 * TODO: implement in luaext_config.c, including with() and the capability
 * presets, and have Sandbox::__construct() read them.
 * ---------------------------------------------------------------------- */

ZEND_METHOD(DevelopGravity_LuaExt_Capabilities, __construct)
{
	LUAEXT_PENDING("DevelopGravity\\LuaExt\\Capabilities::__construct");
}

ZEND_METHOD(DevelopGravity_LuaExt_Capabilities, untrusted)
{
	LUAEXT_PENDING("DevelopGravity\\LuaExt\\Capabilities::untrusted");
}

ZEND_METHOD(DevelopGravity_LuaExt_Capabilities, trusted)
{
	LUAEXT_PENDING("DevelopGravity\\LuaExt\\Capabilities::trusted");
}

ZEND_METHOD(DevelopGravity_LuaExt_Capabilities, with)
{
	LUAEXT_PENDING("DevelopGravity\\LuaExt\\Capabilities::with");
}

ZEND_METHOD(DevelopGravity_LuaExt_Limits, __construct)
{
	LUAEXT_PENDING("DevelopGravity\\LuaExt\\Limits::__construct");
}

ZEND_METHOD(DevelopGravity_LuaExt_Limits, with)
{
	LUAEXT_PENDING("DevelopGravity\\LuaExt\\Limits::with");
}

ZEND_METHOD(DevelopGravity_LuaExt_VfsQuota, __construct)
{
	LUAEXT_PENDING("DevelopGravity\\LuaExt\\VfsQuota::__construct");
}

ZEND_METHOD(DevelopGravity_LuaExt_VfsQuota, with)
{
	LUAEXT_PENDING("DevelopGravity\\LuaExt\\VfsQuota::with");
}

ZEND_METHOD(DevelopGravity_LuaExt_SandboxConfig, __construct)
{
	LUAEXT_PENDING("DevelopGravity\\LuaExt\\SandboxConfig::__construct");
}

ZEND_METHOD(DevelopGravity_LuaExt_SandboxConfig, with)
{
	LUAEXT_PENDING("DevelopGravity\\LuaExt\\SandboxConfig::with");
}

/* -------------------------------------------------------------------------
 * Usage reporting
 *
 * TODO: implement alongside the allocator, the watchdog and the output sink,
 * which is where every field comes from.
 * ---------------------------------------------------------------------- */

ZEND_METHOD(DevelopGravity_LuaExt_SandboxStats, __construct)
{
	LUAEXT_PENDING("DevelopGravity\\LuaExt\\SandboxStats::__construct");
}

ZEND_METHOD(DevelopGravity_LuaExt_SandboxStats, jsonSerialize)
{
	LUAEXT_PENDING("DevelopGravity\\LuaExt\\SandboxStats::jsonSerialize");
}

/* -------------------------------------------------------------------------
 * Host integration value objects
 *
 * TODO: FileStat and ModuleSource are plain data carriers for the VFS and the
 * module loader; implement them with those subsystems.
 * ---------------------------------------------------------------------- */

ZEND_METHOD(DevelopGravity_LuaExt_FileStat, __construct)
{
	LUAEXT_PENDING("DevelopGravity\\LuaExt\\FileStat::__construct");
}

ZEND_METHOD(DevelopGravity_LuaExt_ModuleSource, __construct)
{
	LUAEXT_PENDING("DevelopGravity\\LuaExt\\ModuleSource::__construct");
}

/*
 * TODO: implement with registerObject(), which is what reads the attribute.
 */
ZEND_METHOD(DevelopGravity_LuaExt_LuaMethod, __construct)
{
	LUAEXT_PENDING("DevelopGravity\\LuaExt\\LuaMethod::__construct");
}

/* -------------------------------------------------------------------------
 * Compiled function handle
 *
 * TODO: implement in luaext_function_obj.c, together with the registry ref
 * slots it borrows from its sandbox. Nothing hands out an instance yet, so
 * these are unreachable rather than merely unimplemented.
 * ---------------------------------------------------------------------- */

ZEND_METHOD(DevelopGravity_LuaExt_LuaFunction, __construct)
{
	LUAEXT_PENDING("DevelopGravity\\LuaExt\\LuaFunction::__construct");
}

ZEND_METHOD(DevelopGravity_LuaExt_LuaFunction, call)
{
	LUAEXT_PENDING("DevelopGravity\\LuaExt\\LuaFunction::call");
}

ZEND_METHOD(DevelopGravity_LuaExt_LuaFunction, __invoke)
{
	LUAEXT_PENDING("DevelopGravity\\LuaExt\\LuaFunction::__invoke");
}

ZEND_METHOD(DevelopGravity_LuaExt_LuaFunction, dump)
{
	LUAEXT_PENDING("DevelopGravity\\LuaExt\\LuaFunction::dump");
}

ZEND_METHOD(DevelopGravity_LuaExt_LuaFunction, getSandbox)
{
	LUAEXT_PENDING("DevelopGravity\\LuaExt\\LuaFunction::getSandbox");
}

ZEND_METHOD(DevelopGravity_LuaExt_LuaFunction, isValid)
{
	LUAEXT_PENDING("DevelopGravity\\LuaExt\\LuaFunction::isValid");
}
