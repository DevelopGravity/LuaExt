/*
 * luaext — the LuaFunction handle.
 *
 * A handle is two things: a slot in one sandbox's registry table, and a
 * reference to that sandbox. Both are load bearing. The slot is what keeps the
 * function reachable from the collector's point of view, and the reference is
 * what stops the interpreter holding the slot from being torn down underneath
 * the handle -- a handle onto a closed sandbox names a registry that no longer
 * exists.
 *
 * Dumping to bytecode is the one operation still missing; it is the other half
 * of the loadBytecode/dumpBytecode pair and lands with them.
 */

#include "luaext_function.h"

#include "luaext_convert.h"
#include "luaext_exec.h"
#include "luaext_sandbox.h"

#include <lauxlib.h>
#include <lua.h>

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

/* -------------------------------------------------------------------------
 * Methods
 * ---------------------------------------------------------------------- */

/*
 * The sandbox behind a handle, or NULL with an exception thrown.
 *
 * A handle can outlive what it names in two ways: the sandbox can be closed, or
 * the slot can have been invalidated. Both are reported as a closed sandbox
 * rather than as a broken handle, because that is what actually happened and
 * because ClosedSandboxError is what the stub documents.
 */
static luaext_sandbox *luaext_function_sandbox(luaext_function_obj *function)
{
	luaext_sandbox *sandbox;

	if (Z_TYPE(function->sandbox_zv) != IS_OBJECT) {
		zend_throw_exception(luaext_ce_closed_sandbox_error,
							 "This LuaFunction no longer references a sandbox", 0);
		return NULL;
	}

	sandbox = Z_LUAEXT_SANDBOX_P(&function->sandbox_zv);

	/* Thread affinity first, then open state: exactly the order every Sandbox
	 * method uses, because a handle is a second door onto the same state. */
	if (!luaext_sandbox_check_usable(sandbox)) {
		return NULL;
	}

	if (function->ref < 0) {
		zend_throw_exception(luaext_ce_closed_sandbox_error,
							 "This LuaFunction no longer references a Lua function", 0);
		return NULL;
	}

	return sandbox;
}

static void luaext_function_invoke(zval *this_zv, zval *return_value, zval *args, uint32_t argc)
{
	luaext_function_obj *function = Z_LUAEXT_FUNCTION_P(this_zv);
	luaext_sandbox *sandbox = luaext_function_sandbox(function);
	lua_State *L;

	if (sandbox == NULL) {
		RETURN_THROWS();
	}

	L = sandbox->L;

	if (!lua_checkstack(L, 2)) {
		zend_throw_exception(luaext_ce_runtime_error,
							 "Cannot call a LuaFunction: the interpreter stack cannot grow", 0);
		RETURN_THROWS();
	}

	luaext_convert_ref_push(sandbox, L, function->ref);

	/*
	 * The slot is checked rather than trusted. It is cleared when the handle is
	 * released, and a stale index would otherwise call whatever value was
	 * handed the slot next.
	 */
	if (!lua_isfunction(L, -1)) {
		lua_pop(L, 1);
		zend_throw_exception(luaext_ce_closed_sandbox_error,
							 "This LuaFunction no longer references a Lua function", 0);
		RETURN_THROWS();
	}

	if (!luaext_exec_pcall(sandbox, -1, args, argc, return_value)) {
		RETURN_THROWS();
	}
}

ZEND_METHOD(DevelopGravity_LuaExt_LuaFunction, __construct)
{
	/*
	 * Declared private in the stub, so this is only reachable through
	 * reflection. A handle built that way would carry ref -1 and no sandbox,
	 * which every method already refuses; saying so here is clearer than
	 * letting it fail later as though it had merely expired.
	 */
	zend_throw_error(NULL, "A LuaFunction is obtained from a Sandbox, never constructed directly");
	RETURN_THROWS();
}

ZEND_METHOD(DevelopGravity_LuaExt_LuaFunction, call)
{
	zval *args = NULL;
	uint32_t argc = 0;

	ZEND_PARSE_PARAMETERS_START(0, -1)
	Z_PARAM_VARIADIC('*', args, argc)
	ZEND_PARSE_PARAMETERS_END();

	luaext_function_invoke(ZEND_THIS, return_value, args, argc);
}

ZEND_METHOD(DevelopGravity_LuaExt_LuaFunction, __invoke)
{
	zval *args = NULL;
	uint32_t argc = 0;

	ZEND_PARSE_PARAMETERS_START(0, -1)
	Z_PARAM_VARIADIC('*', args, argc)
	ZEND_PARSE_PARAMETERS_END();

	luaext_function_invoke(ZEND_THIS, return_value, args, argc);
}

ZEND_METHOD(DevelopGravity_LuaExt_LuaFunction, getSandbox)
{
	luaext_function_obj *function;

	ZEND_PARSE_PARAMETERS_NONE();

	function = Z_LUAEXT_FUNCTION_P(ZEND_THIS);

	if (Z_TYPE(function->sandbox_zv) != IS_OBJECT) {
		zend_throw_exception(luaext_ce_closed_sandbox_error,
							 "This LuaFunction no longer references a sandbox", 0);
		RETURN_THROWS();
	}

	/* Returned even when closed: the return type is not nullable, and a closed
	 * sandbox answers isClosed() perfectly well. */
	RETURN_COPY(&function->sandbox_zv);
}

ZEND_METHOD(DevelopGravity_LuaExt_LuaFunction, isValid)
{
	const luaext_function_obj *function;

	ZEND_PARSE_PARAMETERS_NONE();

	function = Z_LUAEXT_FUNCTION_P(ZEND_THIS);

	/*
	 * Deliberately silent about thread affinity: this is the question a caller
	 * asks *instead of* risking an exception, so it must not throw one. It
	 * reads nothing but two fields, neither of which the interpreter owns.
	 */
	if (function->ref < 0 || Z_TYPE(function->sandbox_zv) != IS_OBJECT) {
		RETURN_FALSE;
	}

	RETURN_BOOL(!Z_LUAEXT_SANDBOX_P(&function->sandbox_zv)->closed);
}
