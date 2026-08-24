/*
 * luaext — the PHP side of the boundary.
 *
 * One rule shapes this file, and it is the one the docs promise: a callback
 * that throws a RuntimeError is the host saying "the script is meant to handle
 * this", so the script may catch it; a callback that throws anything else is a
 * failure the script had no part in and must not be able to swallow. Both
 * travel as the *exception object*, never as its message text, so the host
 * catches the class it threw rather than a string that used to be one. The
 * extension this replaces kept only the message, which is why a host could no
 * longer tell a database outage from a validation failure.
 *
 * The second rule is about unwinding. lua_error() is a longjmp: it runs no C
 * cleanup, so a frame holding a zval, an emalloc'd array or a borrowed
 * fcall_info_cache must release all of it before raising. Every function here
 * is arranged so that the raise is the last thing it does, and debug builds
 * assert it through LUAEXT_NO_RAISE_BEGIN/END.
 *
 * The third is ownership. A registered callable outlives the call that
 * registered it, so its fcall_info_cache lives in the closure's own userdata
 * and is released by that userdata's __gc. Lua decides when a closure dies;
 * borrowing the caller's fcc would be a use-after-free the first time it did.
 */

#include "luaext_phpcall.h"

#include "luaext_alloc.h"
#include "luaext_convert.h"
#include "luaext_error.h"
#include "luaext_timers.h"

#include <lauxlib.h>
#include <lua.h>

#include <string.h>

#include <Zend/zend_attributes.h>
#include <Zend/zend_exceptions.h>

/*
 * Identifies the closure storage. Read before anything else in the userdata is
 * touched, so a userdata belonging to some other subsystem is left alone rather
 * than reinterpreted.
 */
#define LUAEXT_PHPCALL_MAGIC 0x4C58436Bu /* "LXCk" */

/* Stack slots any one step here needs: a function, two arguments, a result. */
#define LUAEXT_PHPCALL_SLOTS 8

/* What an unnamed callable is called in a message. */
#define LUAEXT_PHPCALL_ANONYMOUS "an anonymous host callback"

/* -------------------------------------------------------------------------
 * Closure storage
 *
 * A full userdata rather than a light one: only a full userdata can carry a
 * __gc, and __gc is the only thing that can tell us when Lua has finished with
 * a callable the host handed over.
 * ---------------------------------------------------------------------- */

typedef struct {
	uint32_t magic;

	/*
	 * Owned. zend_fcc_dup() takes references on the bound object and closure
	 * and copies a trampoline out of EG(trampoline) if the callable resolved to
	 * one, so this survives the call that registered it and stays valid across
	 * repeated invocations.
	 */
	zend_fcall_info_cache fcc;

	/*
	 * The name the host gave this callable, for messages. Persistent rather
	 * than request-allocated for the same reason the error subsystem's message
	 * is: __gc also runs from lua_close() during the request-shutdown sweep.
	 */
	zend_string *name;
} luaext_phpcall_ud;

/*
 * Release what the storage owns outside the Lua heap.
 *
 * Runs from the collector and from lua_close(). Releasing the fcc can drop the
 * last reference to the bound object and therefore run a PHP destructor; see
 * the note on re-entrancy in luaext_phpcall_metatable().
 */
static int luaext_phpcall_release(lua_State *L)
{
	luaext_phpcall_ud *slot = (luaext_phpcall_ud *)lua_touserdata(L, 1);

	if (slot == NULL || slot->magic != LUAEXT_PHPCALL_MAGIC) {
		return 0;
	}

	/* A finalised closure is no longer one of ours, even if it is resurrected. */
	slot->magic = 0;

	if (ZEND_FCC_INITIALIZED(slot->fcc)) {
		zend_fcc_dtor(&slot->fcc);
	}

	if (slot->name != NULL) {
		zend_string_release(slot->name);
		slot->name = NULL;
	}

	return 0;
}

/*
 * Push the metatable shared by every closure storage in this state, creating it
 * on first use.
 *
 * A metatable *is* its __gc, so the registry key names one payload type: a
 * userdata carrying this metatable must be a luaext_phpcall_ud. A subsystem
 * that wants to hang a different payload off Lua's collector needs its own key,
 * because handing its userdata this __gc would silently leak whatever that
 * payload owns.
 *
 * __metatable is set for the same reason the error subsystem sets it: a script
 * that could read this table back out could replace __gc, and a script that
 * could stamp it onto a value of its own could hand the collector a pointer to
 * memory we never allocated.
 */
static void luaext_phpcall_metatable(lua_State *L)
{
	if (lua_rawgetp(L, LUA_REGISTRYINDEX, &luaext_key_zvalmt) == LUA_TTABLE) {
		return;
	}

	lua_pop(L, 1);
	lua_createtable(L, 0, 2);

	lua_pushcfunction(L, luaext_phpcall_release);
	lua_setfield(L, -2, "__gc");

	lua_pushboolean(L, 0);
	lua_setfield(L, -2, "__metatable");

	lua_pushvalue(L, -1);
	lua_rawsetp(L, LUA_REGISTRYINDEX, &luaext_key_zvalmt);
}

/* What to call this callable in a message. Never NULL. */
static const char *luaext_phpcall_label(const luaext_phpcall_ud *slot)
{
	if (slot == NULL || slot->name == NULL || ZSTR_LEN(slot->name) == 0) {
		return LUAEXT_PHPCALL_ANONYMOUS;
	}

	return ZSTR_VAL(slot->name);
}

/* -------------------------------------------------------------------------
 * Calling PHP from Lua
 * ---------------------------------------------------------------------- */

/*
 * Convert the callback's return value onto the Lua stack, under a protected
 * call.
 *
 * luaext_convert_push_zval() reports failure by raising, and the caller is
 * still holding the return value and the argument array when it does. Running
 * it here means that raise unwinds no further than the caller's own lua_pcall,
 * which then releases everything and re-raises deliberately.
 *
 * Arguments: 1 = the owning sandbox, 2 = the zval to convert.
 */
static int luaext_phpcall_push_result(lua_State *L)
{
	luaext_sandbox *sandbox = (luaext_sandbox *)lua_touserdata(L, 1);
	zval *result = (zval *)lua_touserdata(L, 2);

	lua_settop(L, 0);
	luaext_convert_push_zval(sandbox, L, result);

	return 1;
}

/*
 * The C closure every registered callable is reached through.
 *
 * One PHP return value becomes one Lua value: a string returns a string, an
 * array returns a table. The extension this replaces required a callback to
 * wrap even a single result in an array and warned if it did not, which made
 * the common case the awkward one; a script that genuinely wants several values
 * out of one call unpacks the table it was given.
 */
static int luaext_phpcall_invoke(lua_State *L)
{
	luaext_sandbox *sandbox = LUAEXT_SB(L);
	luaext_phpcall_ud *slot = (luaext_phpcall_ud *)lua_touserdata(L, lua_upvalueindex(1));

	/* Only read out of the storage once the storage has been vouched for: a
	 * userdata that is not ours has no name field to read. */
	const char *label = LUAEXT_PHPCALL_ANONYMOUS;
	uint32_t depth_limit;
	zval *params = NULL;
	size_t params_bytes = 0;
	zval result;
	int argc = lua_gettop(L);
	int index;
	int status = LUA_OK;
	bool converted = true;

	/*
	 * Everything up to the argument loop owns nothing, so it may raise freely.
	 * Past it, the frame owns zvals and host memory and must not.
	 */

	if (slot == NULL || slot->magic != LUAEXT_PHPCALL_MAGIC || !ZEND_FCC_INITIALIZED(slot->fcc)) {
		luaext_error_raise(L, LUAEXT_ERR_ABORT, true,
						   "A host callback was invoked after its storage was released");
	}

	label = luaext_phpcall_label(slot);

	if (sandbox == NULL || sandbox->closed || sandbox->L == NULL) {
		luaext_error_raise(L, LUAEXT_ERR_ABORT, true,
						   "The host callback %s cannot run: its sandbox is gone", label);
	}

	/*
	 * An exception already in flight would be indistinguishable from one this
	 * callback threw, and calling PHP with one pending is undefined anyway.
	 * Converting it here keeps the classification honest.
	 */
	if (EG(exception) != NULL) {
		luaext_error_raise_from_exception(L);
	}

	/*
	 * Bounds how deeply Lua and PHP may call each other. The interpreter's own
	 * C-call ceiling would eventually stop unbounded recursion as an untyped
	 * "C stack overflow", so this is not the only guard -- but it is the one the
	 * host configured, and it names what actually happened.
	 */
	depth_limit = sandbox->policy.limits.max_call_depth;

	if (depth_limit != 0 && sandbox->in_php >= 0 && (uint32_t)sandbox->in_php >= depth_limit) {
		luaext_error_raise(L, LUAEXT_ERR_ABORT, true,
						   "The host callback %s was refused: calls across the PHP boundary are "
						   "already nested %u deep",
						   label, depth_limit);
	}

	if (!lua_checkstack(L, LUAEXT_PHPCALL_SLOTS)) {
		luaext_error_raise(L, LUAEXT_ERR_ABORT, true,
						   "The host callback %s was refused: the interpreter stack cannot grow",
						   label);
	}

	/*
	 * The argument array is host memory the script caused to be allocated, and
	 * lua_Alloc never sees it. Billing it against the same ceiling is what stops
	 * a script from spending host memory it has no budget for by calling out
	 * with an enormous argument list.
	 */
	if (argc > 0) {
		params_bytes = (size_t)argc * sizeof(zval);

		if (!luaext_alloc_charge(sandbox, params_bytes)) {
			luaext_error_raise(L, LUAEXT_ERR_MEMORY, true,
							   "The host callback %s was refused: its %d argument(s) do not fit in "
							   "the sandbox's memory budget",
							   label, argc);
		}

		params = (zval *)safe_emalloc((size_t)argc, sizeof(zval), 0);
	}

	ZVAL_UNDEF(&result);

	LUAEXT_NO_RAISE_BEGIN(L);

	/* Valid before anything can fail, so every slot is releasable either way. */
	for (index = 0; index < argc; index++) {
		ZVAL_UNDEF(&params[index]);
	}

	for (index = 0; index < argc && converted; index++) {
		converted = luaext_convert_to_zval(sandbox, L, index + 1, &params[index]);
	}

	if (converted) {
		/*
		 * The boundary counters. in_php is what lets a later wave stop charging
		 * CPU to the script while the host works, and what makes a nested
		 * callback's depth accounting correct; php_calls_out is what stats()
		 * reports.
		 */
		sandbox->php_calls_out++;
		sandbox->in_php++;

		/*
		 * zend_call_known_fcc() rather than zend_call_function(): it copies a
		 * trampoline before calling, because zend_call_function() frees the one
		 * it is given, and the fcc it would be given here is the closure's own
		 * long-lived copy.
		 */
		zend_call_known_fcc(&slot->fcc, &result, (uint32_t)argc, params, NULL);

		sandbox->in_php--;

		/*
		 * A callback that paused its own billing and forgot to resume does not
		 * get to keep the pause. Note what this does NOT do: it does not pause
		 * around the call. Time a host callback spends is the script's doing and
		 * is billed by default; only an explicit pauseTimers() un-bills it, and
		 * only when every enclosing frame paused too.
		 *
		 * A zend_bailout inside the callback longjmps past this, leaving the
		 * pause outstanding as well as the in_php increment the same bailout
		 * already stranded. That is the existing tracked hazard, not a new one:
		 * a leaked pause errs towards NOT billing, so it is the one direction
		 * worth naming out loud.
		 */
		luaext_timers_php_returned(sandbox);
	}

	/*
	 * A thrown exception is the answer, so there is nothing to convert. Checked
	 * explicitly rather than inferred from the return value: an exception must
	 * never reach the interpreter as anything but a deliberate conversion.
	 */
	if (EG(exception) == NULL && converted) {
		lua_pushcfunction(L, luaext_phpcall_push_result);
		lua_pushlightuserdata(L, sandbox);
		lua_pushlightuserdata(L, &result);

		/*
		 * Lifted for exactly the length of the protected call. A raise inside it
		 * unwinds to this lua_pcall and no further, so it strands nothing this
		 * frame owns -- which is precisely what the assertion exists to check
		 * everywhere else.
		 */
		LUAEXT_NO_RAISE_END(L);
		status = lua_pcall(L, 2, 1, 0);
		LUAEXT_NO_RAISE_BEGIN(L);
	}

	if (Z_TYPE(result) != IS_UNDEF) {
		zval_ptr_dtor(&result);
		ZVAL_UNDEF(&result);
	}

	for (index = 0; index < argc; index++) {
		zval_ptr_dtor(&params[index]);
	}

	if (params != NULL) {
		efree(params);
		params = NULL;
		luaext_alloc_discharge(sandbox, params_bytes);
	}

	LUAEXT_NO_RAISE_END(L);

	/* Nothing is owned from here down, so raising is finally safe. */

	/*
	 * The callback boundary, tier 3 of interrupt delivery. A limit that expired
	 * while the host was working gets delivered here rather than waiting for the
	 * script to execute another instruction -- which matters most for the
	 * callback that never returns to Lua at all because it is the last thing the
	 * script does.
	 */
	LUAEXT_CHECK(L);

	if (EG(exception) != NULL) {
		/*
		 * Retains the object and decides catchable versus fatal from its class.
		 * Does not return.
		 */
		luaext_error_raise_from_exception(L);
	}

	if (status != LUA_OK) {
		/* The failed conversion left its error value on top; re-raise it
		 * unchanged so the host sees the ConversionError it describes. */
		return lua_error(L);
	}

	/*
	 * Unreachable while the conversion subsystem keeps its promise to throw on
	 * every failure. Returning here would hand the script the last argument as
	 * though it were the result, so the promise is checked rather than trusted.
	 */
	if (!converted) {
		luaext_error_raise(L, LUAEXT_ERR_CONVERSION, true,
						   "The arguments to the host callback %s could not be converted", label);
	}

	return 1;
}

/* -------------------------------------------------------------------------
 * Exposing a callable
 * ---------------------------------------------------------------------- */

typedef struct {
	zend_fcall_info_cache fcc;
	const char *name;
	size_t name_len;
} luaext_phpcall_build;

/*
 * The allocating half of building a closure, run under lua_pcall.
 *
 * Every step here can raise on a memory error, and the callers are PHP method
 * bodies where a raise has nothing to unwind to -- it would reach lua_atpanic
 * and take the request with it. Protecting the whole build turns that into a
 * thrown exception.
 *
 * Argument 1: the build request.
 */
static int luaext_phpcall_build_closure(lua_State *L)
{
	luaext_phpcall_build *build = (luaext_phpcall_build *)lua_touserdata(L, 1);
	luaext_phpcall_ud *slot;

	lua_settop(L, 0);
	luaL_checkstack(L, LUAEXT_PHPCALL_SLOTS, "luaext: no stack to build a host callback");

	slot = (luaext_phpcall_ud *)lua_newuserdatauv(L, sizeof(*slot), 0);

	memset(slot, 0, sizeof(*slot));
	slot->magic = LUAEXT_PHPCALL_MAGIC;
	slot->fcc = empty_fcall_info_cache;

	/*
	 * __gc is armed before the storage owns anything. A raise from any step
	 * below then still leaves a userdata the collector will finalise, rather
	 * than one holding references nothing will ever release.
	 */
	luaext_phpcall_metatable(L);
	lua_setmetatable(L, -2);

	zend_fcc_dup(&slot->fcc, &build->fcc);

	if (build->name != NULL && build->name_len > 0) {
		slot->name = zend_string_init(build->name, build->name_len, 1);
	}

	lua_pushcclosure(L, luaext_phpcall_invoke, 1);

	return 1;
}

/*
 * Turn the Lua error a protected build failed with into a thrown PHP exception.
 *
 * Only a memory error can get here, so the class is the honest one rather than
 * a generic failure. lua_tostring() rather than luaL_tolstring(): this runs
 * outside any protected call, and a __tostring metamethod must not be given the
 * chance to raise where nothing would catch it.
 */
static void luaext_phpcall_throw_lua_failure(lua_State *L, const char *what, const char *name)
{
	const char *message = lua_tostring(L, -1);

	zend_throw_exception_ex(luaext_ce_memory_limit_error, 0, "Cannot %s \"%s\": %s", what,
							name != NULL ? name : LUAEXT_PHPCALL_ANONYMOUS,
							message != NULL ? message : "the interpreter ran out of memory");

	lua_pop(L, 1);
}

/* Reject a sandbox that cannot be registered into. */
static bool luaext_phpcall_usable(const luaext_sandbox *sandbox)
{
	if (sandbox == NULL || sandbox->closed || sandbox->L == NULL) {
		zend_throw_exception(luaext_ce_closed_sandbox_error, "The sandbox has been closed", 0);
		return false;
	}

	return true;
}

/*
 * Resolve `callable` into `fcc`, throwing a ConfigurationError naming `name` if
 * it is not callable.
 *
 * The engine's own wording is reused for the reason, so a host reads the same
 * explanation it would get from any other callable parameter.
 */
static bool luaext_phpcall_resolve(zval *callable, const char *name, zend_fcall_info_cache *fcc)
{
	char *reason = NULL;
	bool callable_ok;

	*fcc = empty_fcall_info_cache;
	callable_ok = zend_is_callable_ex(callable, NULL, 0, NULL, fcc, &reason);

	if (!callable_ok) {
		zend_throw_exception_ex(luaext_ce_configuration_error, 0,
								"Cannot expose \"%s\" to Lua: to be a valid callback, %s",
								name != NULL ? name : LUAEXT_PHPCALL_ANONYMOUS,
								reason != NULL ? reason : "it must be callable");
	}

	/* Set on some successful resolutions too, as a deprecation note. */
	if (reason != NULL) {
		efree(reason);
	}

	return callable_ok;
}

bool luaext_phpcall_push(luaext_sandbox *sandbox, zval *callable, const char *name)
{
	luaext_phpcall_build build;
	lua_State *L;

	if (!luaext_phpcall_usable(sandbox)) {
		return false;
	}

	if (callable == NULL) {
		zend_throw_exception(luaext_ce_configuration_error, "Cannot expose a missing value to Lua",
							 0);
		return false;
	}

	if (!luaext_phpcall_resolve(callable, name, &build.fcc)) {
		return false;
	}

	build.name = name;
	build.name_len = name != NULL ? strlen(name) : 0;

	L = sandbox->L;

	if (!lua_checkstack(L, LUAEXT_PHPCALL_SLOTS)) {
		zend_throw_exception(
			luaext_ce_memory_limit_error,
			"Cannot expose a PHP callable to Lua: the interpreter stack cannot grow", 0);
		return false;
	}

	lua_pushcfunction(L, luaext_phpcall_build_closure);
	lua_pushlightuserdata(L, &build);

	if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
		luaext_phpcall_throw_lua_failure(L, "expose a PHP callable to Lua as", name);
		return false;
	}

	return true;
}

/* -------------------------------------------------------------------------
 * Exposing a table of callables
 * ---------------------------------------------------------------------- */

typedef struct {
	luaext_sandbox *sandbox;
	const char *name;
	size_t name_len;
	HashTable *functions;

	/* A PHP exception was thrown inside the protected build; the Lua side
	 * returned normally, so the status alone would not show it. */
	bool failed;
} luaext_phpcall_table;

/*
 * Build the library table and publish it, under lua_pcall.
 *
 * Argument 1: the registration request.
 */
static int luaext_phpcall_build_table(lua_State *L)
{
	luaext_phpcall_table *build = (luaext_phpcall_table *)lua_touserdata(L, 1);
	zend_string *key;
	zval *entry;

	lua_settop(L, 0);
	luaL_checkstack(L, LUAEXT_PHPCALL_SLOTS, "luaext: no stack to build a library table");

	/*
	 * Raw access throughout. The globals table may later carry a metatable that
	 * a script is not allowed to see through, and a host registering a library
	 * is not the caller that metatable exists to constrain.
	 */
	lua_pushglobaltable(L);
	lua_pushlstring(L, build->name, build->name_len);
	lua_rawget(L, -2);

	/* Adding to an existing library rather than replacing it, so two calls can
	 * build one namespace; anything that is not a table is replaced. */
	if (!lua_istable(L, -1)) {
		lua_pop(L, 1);
		lua_createtable(L, 0, (int)zend_hash_num_elements(build->functions));
	}

	ZEND_HASH_FOREACH_STR_KEY_VAL(build->functions, key, entry)
	{
		if (!luaext_phpcall_push(build->sandbox, entry, ZSTR_VAL(key))) {
			/* The global is assigned last, so abandoning here leaves the
			 * interpreter without a half-built library in it. */
			build->failed = true;
			return 0;
		}

		lua_pushlstring(L, ZSTR_VAL(key), ZSTR_LEN(key));
		lua_insert(L, -2);
		lua_rawset(L, -3);
	}
	ZEND_HASH_FOREACH_END();

	lua_pushlstring(L, build->name, build->name_len);
	lua_insert(L, -2);
	lua_rawset(L, -3);

	return 0;
}

/*
 * Refuse a table of callables before a single Lua object is built for it, so a
 * rejected registration leaves the interpreter exactly as it was.
 */
static bool luaext_phpcall_check_functions(HashTable *functions)
{
	zend_string *key;
	zval *entry;

	if (functions == NULL || zend_hash_num_elements(functions) == 0) {
		zend_throw_exception(luaext_ce_configuration_error,
							 "A Lua library must expose at least one callable", 0);
		return false;
	}

	ZEND_HASH_FOREACH_STR_KEY_VAL(functions, key, entry)
	{
		if (key == NULL || ZSTR_LEN(key) == 0) {
			zend_throw_exception(
				luaext_ce_configuration_error,
				"Every entry of a Lua library must be keyed by the non-empty name Lua will see", 0);
			return false;
		}

		if (!zend_is_callable(entry, 0, NULL)) {
			zend_throw_exception_ex(luaext_ce_configuration_error, 0,
									"The Lua library entry \"%s\" is not a valid callback",
									ZSTR_VAL(key));
			return false;
		}
	}
	ZEND_HASH_FOREACH_END();

	return true;
}

bool luaext_phpcall_register_table(luaext_sandbox *sandbox, const char *name, size_t name_len,
								   HashTable *functions)
{
	luaext_phpcall_table build;
	lua_State *L;

	if (!luaext_phpcall_usable(sandbox)) {
		return false;
	}

	if (name == NULL || name_len == 0) {
		zend_throw_exception(luaext_ce_configuration_error,
							 "A Lua library needs a name for scripts to reach it by", 0);
		return false;
	}

	if (!luaext_phpcall_check_functions(functions)) {
		return false;
	}

	build.sandbox = sandbox;
	build.name = name;
	build.name_len = name_len;
	build.functions = functions;
	build.failed = false;

	L = sandbox->L;

	if (!lua_checkstack(L, LUAEXT_PHPCALL_SLOTS)) {
		zend_throw_exception(luaext_ce_memory_limit_error,
							 "Cannot register a Lua library: the interpreter stack cannot grow", 0);
		return false;
	}

	lua_pushcfunction(L, luaext_phpcall_build_table);
	lua_pushlightuserdata(L, &build);

	if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
		luaext_phpcall_throw_lua_failure(L, "register the Lua library", name);
		return false;
	}

	return !build.failed;
}

/* -------------------------------------------------------------------------
 * Selecting an object's methods
 *
 * Explicit only, in both directions. A host that adds a public method to a
 * class it happens to have registered must not thereby widen what untrusted
 * code may call, which is why there is no "expose everything public" mode and
 * why a class with neither an allowlist nor an attribute is an error rather
 * than an empty table.
 * ---------------------------------------------------------------------- */

/* Why a method cannot be exposed, or NULL when it can. */
static const char *luaext_phpcall_method_refusal(const zend_function *method)
{
	uint32_t flags = method->common.fn_flags;
	const zend_string *method_name = method->common.function_name;

	if ((flags & ZEND_ACC_PUBLIC) == 0) {
		return "it is not public";
	}

	if ((flags & ZEND_ACC_STATIC) != 0) {
		return "it is static, and only bound instance methods cross this boundary";
	}

	if ((flags & ZEND_ACC_ABSTRACT) != 0) {
		return "it is abstract";
	}

	/*
	 * Magic methods are refused wholesale, and __call is the reason. Exposing it
	 * would turn one entry in an allowlist into every name the class can be
	 * asked for, which is exactly the implicit surface this bridge exists to
	 * avoid. The others -- __get, __destruct, __toString -- are no better as
	 * script-callable entry points.
	 */
	if (method_name != NULL && ZSTR_LEN(method_name) >= 2 && ZSTR_VAL(method_name)[0] == '_' &&
		ZSTR_VAL(method_name)[1] == '_') {
		return "it is a magic method, and magic methods are never exposed";
	}

	return NULL;
}

/*
 * Add `method`, bound to `instance`, under the name Lua will see.
 *
 * The callable is an array pair rather than a closure: the pair is what the
 * header promises, it keeps the instance alive by ordinary refcounting, and it
 * resolves to the exact zend_function this selection already vetted.
 */
static bool luaext_phpcall_add_method(HashTable *methods, zval *instance, zend_function *method,
									  zend_string *lua_name)
{
	const zend_class_entry *scope = method->common.scope;
	zval callable;
	zval bound;

	array_init_size(&callable, 2);

	ZVAL_COPY(&bound, instance);
	add_next_index_zval(&callable, &bound);
	add_next_index_str(&callable, zend_string_copy(method->common.function_name));

	if (zend_hash_add(methods, lua_name, &callable) == NULL) {
		zval_ptr_dtor(&callable);
		zend_throw_exception_ex(
			luaext_ce_configuration_error, 0, "Two methods of %s both want the Lua name \"%s\"",
			scope != NULL ? ZSTR_VAL(scope->name) : "the object", ZSTR_VAL(lua_name));
		return false;
	}

	return true;
}

/*
 * The name #[LuaMethod] asks for, or the method's own name.
 *
 * The attribute is instantiated rather than read out of its argument list, so
 * `#[LuaMethod('query')]` and `#[LuaMethod(name: 'query')]` and a constant
 * expression all behave the way the host wrote them.
 *
 * Returns a reference the caller releases, or false with an exception thrown.
 */
static bool luaext_phpcall_attribute_name(zend_attribute *attribute, zend_function *method,
										  zend_string **out)
{
	zend_class_entry *scope = method->common.scope;
	zend_string *filename = NULL;
	zval marker;
	zval holder;
	zval *configured;

	if (scope != NULL && scope->type == ZEND_USER_CLASS) {
		filename = scope->info.user.filename;
	}

	if (zend_get_attribute_object(&marker, luaext_ce_lua_method_attribute, attribute, scope,
								  filename) != SUCCESS) {
		return false;
	}

	configured = zend_read_property(luaext_ce_lua_method_attribute, Z_OBJ(marker),
									ZEND_STRL("name"), true, &holder);

	if (configured != NULL && Z_TYPE_P(configured) == IS_STRING) {
		if (Z_STRLEN_P(configured) == 0) {
			zval_ptr_dtor(&marker);
			zend_throw_exception_ex(luaext_ce_configuration_error, 0,
									"%s::%s() carries a #[LuaMethod] with an empty name",
									scope != NULL ? ZSTR_VAL(scope->name) : "?",
									ZSTR_VAL(method->common.function_name));
			return false;
		}

		/* Copied before the marker is released: it owns the string. */
		*out = zend_string_copy(Z_STR_P(configured));
	} else {
		*out = zend_string_copy(method->common.function_name);
	}

	zval_ptr_dtor(&marker);

	return true;
}

/* The caller's explicit allowlist, which overrides every attribute. */
static bool luaext_phpcall_collect_allowlist(HashTable *methods, zval *instance,
											 zend_class_entry *ce, HashTable *allowlist)
{
	zval *entry;

	ZEND_HASH_FOREACH_VAL(allowlist, entry)
	{
		zend_string *requested;
		zend_string *lowered;
		zend_function *method;
		const char *refusal;

		ZVAL_DEREF(entry);

		if (Z_TYPE_P(entry) != IS_STRING || Z_STRLEN_P(entry) == 0) {
			zend_throw_exception_ex(luaext_ce_configuration_error, 0,
									"The method allowlist for %s must hold non-empty method names",
									ZSTR_VAL(ce->name));
			return false;
		}

		requested = Z_STR_P(entry);
		lowered = zend_string_tolower(requested);

		/*
		 * Looked up in the class's own table rather than resolved as a callable:
		 * a name the class does not declare must be an error, not a silent
		 * detour through __call.
		 */
		method = (zend_function *)zend_hash_find_ptr(&ce->function_table, lowered);
		zend_string_release(lowered);

		if (method == NULL) {
			zend_throw_exception_ex(luaext_ce_configuration_error, 0,
									"%s has no method %s() to expose to Lua", ZSTR_VAL(ce->name),
									ZSTR_VAL(requested));
			return false;
		}

		refusal = luaext_phpcall_method_refusal(method);

		if (refusal != NULL) {
			zend_throw_exception_ex(luaext_ce_configuration_error, 0,
									"%s::%s() cannot be exposed to Lua: %s", ZSTR_VAL(ce->name),
									ZSTR_VAL(requested), refusal);
			return false;
		}

		/* The name as the host wrote it, so an allowlist reads the same on both
		 * sides of the boundary. */
		if (!luaext_phpcall_add_method(methods, instance, method, requested)) {
			return false;
		}
	}
	ZEND_HASH_FOREACH_END();

	if (zend_hash_num_elements(methods) == 0) {
		zend_throw_exception_ex(luaext_ce_configuration_error, 0,
								"The method allowlist for %s selects no method, so there would be "
								"nothing for a script to call",
								ZSTR_VAL(ce->name));
		return false;
	}

	return true;
}

/* Every method carrying #[LuaMethod], honouring the name it asks for. */
static bool luaext_phpcall_collect_attributed(HashTable *methods, zval *instance,
											  zend_class_entry *ce)
{
	zend_string *marker_name = zend_string_tolower(luaext_ce_lua_method_attribute->name);
	zend_function *method;
	bool collected = true;

	ZEND_HASH_MAP_FOREACH_PTR(&ce->function_table, method)
	{
		zend_attribute *attribute = zend_get_attribute(method->common.attributes, marker_name);
		zend_string *lua_name;
		const char *refusal;

		if (attribute == NULL) {
			continue;
		}

		refusal = luaext_phpcall_method_refusal(method);

		if (refusal != NULL) {
			/* Marked but unexposable is a host mistake, and a silently missing
			 * function is a worse way to find out about it than an exception. */
			zend_throw_exception_ex(
				luaext_ce_configuration_error, 0,
				"%s::%s() carries #[LuaMethod] but cannot be exposed to Lua: %s",
				ZSTR_VAL(ce->name), ZSTR_VAL(method->common.function_name), refusal);
			collected = false;
			break;
		}

		if (!luaext_phpcall_attribute_name(attribute, method, &lua_name)) {
			collected = false;
			break;
		}

		collected = luaext_phpcall_add_method(methods, instance, method, lua_name);
		zend_string_release(lua_name);

		if (!collected) {
			break;
		}
	}
	ZEND_HASH_FOREACH_END();

	zend_string_release(marker_name);

	if (!collected) {
		return false;
	}

	if (zend_hash_num_elements(methods) == 0) {
		zend_throw_exception_ex(
			luaext_ce_configuration_error, 0,
			"No method of %s carries #[LuaMethod] and no allowlist was given, so nothing would be "
			"exposed; selection is always explicit",
			ZSTR_VAL(ce->name));
		return false;
	}

	return true;
}

HashTable *luaext_phpcall_collect_methods(zval *instance, HashTable *allowlist)
{
	zend_class_entry *ce;
	HashTable *methods;
	bool collected;

	if (instance == NULL || Z_TYPE_P(instance) != IS_OBJECT) {
		zend_throw_exception(luaext_ce_configuration_error,
							 "Only an object's methods can be exposed to Lua", 0);
		return NULL;
	}

	ce = Z_OBJCE_P(instance);
	methods = zend_new_array(allowlist != NULL ? zend_hash_num_elements(allowlist) : 8);

	if (allowlist != NULL) {
		collected = luaext_phpcall_collect_allowlist(methods, instance, ce, allowlist);
	} else {
		collected = luaext_phpcall_collect_attributed(methods, instance, ce);
	}

	if (!collected) {
		zend_array_destroy(methods);
		return NULL;
	}

	return methods;
}
