/*
 * luaext — the LuaFunction handle.
 *
 * A LuaFunction is a PHP-side reference to a Lua function held in the owning
 * sandbox's registry. Only the object plumbing lives here in Wave 1; compiling
 * and calling arrive with their own subsystems.
 */

#ifndef LUAEXT_FUNCTION_H
#define LUAEXT_FUNCTION_H

#include "luaext_types.h"

/*
 * Install the LuaFunction object handlers. Called from MINIT once
 * luaext_ce_lua_function exists.
 *
 * Without this the engine allocates a bare zend_object while
 * luaext_function_from_obj() subtracts the offset of a larger
 * luaext_function_obj, so every access would land before the allocation.
 */
void luaext_function_startup(void);

#endif /* LUAEXT_FUNCTION_H */
