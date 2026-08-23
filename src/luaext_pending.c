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
