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

/* -------------------------------------------------------------------------
 * Lua context on a thrown exception
 *
 * These are not pending in the same sense: nothing in the extension raises an
 * exception from inside Lua yet, and the interface documents null for a failure
 * that did not originate there. Returning the documented "no Lua involved"
 * answer is the correct behaviour today rather than a placeholder.
 *
 * TODO: attach the real traceback, chunk name and line when the error machinery
 * lands, and reuse one implementation for both hierarchies.
 * ---------------------------------------------------------------------- */

#define LUAEXT_DEFINE_TRACE_ACCESSORS(base_class)                                                  \
	ZEND_METHOD(base_class, getLuaTrace)                                                           \
	{                                                                                              \
		ZEND_PARSE_PARAMETERS_NONE();                                                              \
		RETURN_NULL();                                                                             \
	}                                                                                              \
                                                                                                   \
	ZEND_METHOD(base_class, getLuaTraceAsString)                                                   \
	{                                                                                              \
		ZEND_PARSE_PARAMETERS_NONE();                                                              \
		RETURN_EMPTY_STRING();                                                                     \
	}                                                                                              \
                                                                                                   \
	ZEND_METHOD(base_class, getSandbox)                                                            \
	{                                                                                              \
		ZEND_PARSE_PARAMETERS_NONE();                                                              \
		RETURN_NULL();                                                                             \
	}                                                                                              \
                                                                                                   \
	ZEND_METHOD(base_class, getChunkName)                                                          \
	{                                                                                              \
		ZEND_PARSE_PARAMETERS_NONE();                                                              \
		RETURN_NULL();                                                                             \
	}                                                                                              \
                                                                                                   \
	ZEND_METHOD(base_class, getLuaLine)                                                            \
	{                                                                                              \
		ZEND_PARSE_PARAMETERS_NONE();                                                              \
		RETURN_NULL();                                                                             \
	}

LUAEXT_DEFINE_TRACE_ACCESSORS(DevelopGravity_LuaExt_Exception_LuaException)
LUAEXT_DEFINE_TRACE_ACCESSORS(DevelopGravity_LuaExt_Exception_LuaLogicException)
