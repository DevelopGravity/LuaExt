/*
 * luaext — the LuaFunction handle.
 *
 * Wave 1 wires the object layout only. The handle carries a reference to the
 * sandbox that owns the function and a slot in that sandbox's registry table;
 * compiling, calling and dumping arrive with their own subsystems.
 */

#include "luaext_function.h"

#include "luaext_convert.h"

#include <Zend/zend_exceptions.h>

static zend_object_handlers luaext_function_handlers;

static zend_object *luaext_function_create_object(zend_class_entry *ce)
{
	luaext_function_obj *function = zend_object_alloc(sizeof(luaext_function_obj), ce);

	zend_object_std_init(&function->std, ce);
	object_properties_init(&function->std, ce);
	function->std.handlers = &luaext_function_handlers;

	ZVAL_UNDEF(&function->sandbox_zv);

	/* No registry slot until a subsystem hands this handle a function. */
	function->ref = -1;

	return &function->std;
}

static void luaext_function_free_object(zend_object *object)
{
	luaext_function_obj *function = luaext_function_from_obj(object);

	/*
	 * Return the registry slot before dropping the sandbox reference, because
	 * releasing it needs the interpreter this handle is the last owner of.
	 *
	 * Skipped once the sandbox is closed: lua_close() has already destroyed the
	 * registry, so there is no table left to write to and nothing to leak.
	 */
	if (function->ref >= 0 && Z_TYPE(function->sandbox_zv) == IS_OBJECT) {
		luaext_sandbox *sandbox = Z_LUAEXT_SANDBOX_P(&function->sandbox_zv);

		if (!sandbox->closed && sandbox->L != NULL) {
			luaext_convert_ref_release(sandbox, sandbox->L, function->ref);
		}
	}

	function->ref = -1;

	zval_ptr_dtor(&function->sandbox_zv);
	ZVAL_UNDEF(&function->sandbox_zv);

	zend_object_std_dtor(object);
}

void luaext_function_startup(void)
{
	memcpy(&luaext_function_handlers, &std_object_handlers, sizeof(zend_object_handlers));

	luaext_function_handlers.offset = XtOffsetOf(luaext_function_obj, std);
	luaext_function_handlers.free_obj = luaext_function_free_object;

	/*
	 * A copy could only ever be a second handle onto one registry slot, which
	 * would then be released twice. Cloning is refused rather than aliased.
	 */
	luaext_function_handlers.clone_obj = NULL;

	luaext_ce_lua_function->create_object = luaext_function_create_object;
}
