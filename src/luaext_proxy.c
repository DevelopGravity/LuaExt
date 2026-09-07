/*
 * luaext — PHP object proxies: the class registry.
 *
 * Registration is where every rule is enforced, so nothing downstream has to
 * re-check: by the time a record exists, its method tables hold only public,
 * non-abstract zend_functions, its table names cannot collide, and its class
 * is a concrete, nameable one. A refusal here leaves the script's view of the
 * world untouched — selection happens entirely before anything is stored.
 *
 * Storage is persistent (pemalloc, persistent strings): the registry is torn
 * down on the close path, which can run from the RSHUTDOWN sweep after the
 * request allocator's arena is on its way out.
 */

#include "luaext_proxy.h"

#include "luaext_defer.h"
#include "luaext_error.h"
#include "luaext_phpcall.h"

#include <lauxlib.h>
#include <lua.h>

#include <string.h>

#include <Zend/zend_attributes.h>
#include <Zend/zend_exceptions.h>

/* -------------------------------------------------------------------------
 * Small helpers
 * ---------------------------------------------------------------------- */

/* A persistent copy of a (possibly request-allocated) string. */
static zend_string *luaext_proxy_pstr(const zend_string *source)
{
	return zend_string_init(ZSTR_VAL(source), ZSTR_LEN(source), 1);
}

/*
 * Why a method cannot be exposed, or NULL. Deliberately NOT the phpcall
 * version: statics are first-class citizens here (they land on the class
 * table), while registerObject() has no receiver-free surface to put one on.
 */
static const char *luaext_proxy_method_refusal(const zend_function *method)
{
	if (!(method->common.fn_flags & ZEND_ACC_PUBLIC)) {
		return "it is not public";
	}

	if (method->common.fn_flags & ZEND_ACC_ABSTRACT) {
		return "it is abstract";
	}

	return NULL;
}

/*
 * Add one selected method under its Lua name, refusing a duplicate key. The
 * instance and static tables are separate namespaces (metatable __index vs
 * the class table), so only same-table duplicates are collisions.
 */
static bool luaext_proxy_table_add(HashTable *table, const zend_class_entry *ce,
								   zend_string *lua_name, zend_function *method)
{
	zend_string *key = luaext_proxy_pstr(lua_name);

	if (zend_hash_add_ptr(table, key, method) == NULL) {
		zend_string_release(key);
		zend_throw_exception_ex(luaext_ce_configuration_error, 0,
								"Two exposures of %s both want the Lua name \"%s\"",
								ZSTR_VAL(ce->name), ZSTR_VAL(lua_name));
		return false;
	}

	/* The table holds its own reference now. */
	zend_string_release(key);

	return true;
}

/* -------------------------------------------------------------------------
 * Selection
 *
 * Two routes, mirroring registerObject(): an explicit allowlist overrides
 * every attribute. Constructors and __toString are routed to their dedicated
 * fields rather than the method tables, whichever route selected them.
 * ---------------------------------------------------------------------- */

/* Record an exposed constructor under `name`, refusing a class-table clash. */
static bool luaext_proxy_set_constructor(luaext_proxy_class *record, const zend_class_entry *ce,
										 zend_function *constructor, zend_string *name)
{
	record->constructor = constructor;
	record->constructor_lua_name = luaext_proxy_pstr(name);

	if (zend_hash_exists(record->static_methods, record->constructor_lua_name)) {
		zend_throw_exception_ex(luaext_ce_configuration_error, 0,
								"Two exposures of %s both want the Lua name \"%s\"",
								ZSTR_VAL(ce->name), ZSTR_VAL(name));
		return false;
	}

	return true;
}

/* Every method carrying #[LuaMethod], honouring the name it asks for. */
static bool luaext_proxy_collect_attributed(luaext_proxy_class *record, zend_class_entry *ce)
{
	zend_string *marker = zend_string_tolower(luaext_ce_lua_method_attribute->name);
	zend_function *method;
	zend_function *constructor = NULL;
	zend_string *constructor_name = NULL;
	bool collected = true;

	ZEND_HASH_MAP_FOREACH_PTR(&ce->function_table, method)
	{
		zend_attribute *attribute = zend_get_attribute(method->common.attributes, marker);
		zend_string *lua_name;
		const char *refusal;
		bool overridden;

		if (attribute == NULL) {
			continue;
		}

		refusal = luaext_proxy_method_refusal(method);

		if (refusal != NULL) {
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

		/* An attribute with no name override resolves to the method's own. */
		overridden = !zend_string_equals(lua_name, method->common.function_name);

		if (method == ce->constructor) {
			constructor = method;

			if (overridden) {
				constructor_name = lua_name;
			} else {
				zend_string_release(lua_name);
				constructor_name = ZSTR_INIT_LITERAL("new", 0);
			}

			continue;
		}

		if (ce->__tostring != NULL && method == ce->__tostring) {
			/* Marking __toString activates the metamethod; a name override
			 * additionally exposes it as an ordinary method. */
			record->to_string = method;

			if (overridden) {
				collected = luaext_proxy_table_add(record->instance_methods, ce, lua_name, method);
			}

			zend_string_release(lua_name);

			if (!collected) {
				break;
			}

			continue;
		}

		if (method->common.fn_flags & ZEND_ACC_STATIC) {
			collected = luaext_proxy_table_add(record->static_methods, ce, lua_name, method);
		} else {
			collected = luaext_proxy_table_add(record->instance_methods, ce, lua_name, method);
		}

		zend_string_release(lua_name);

		if (!collected) {
			break;
		}
	}
	ZEND_HASH_FOREACH_END();

	zend_string_release(marker);

	if (collected && constructor != NULL) {
		collected = luaext_proxy_set_constructor(record, ce, constructor, constructor_name);
	}

	if (constructor_name != NULL) {
		zend_string_release(constructor_name);
	}

	return collected;
}

/* The caller's explicit allowlist, which overrides every attribute. */
static bool luaext_proxy_collect_allowlist(luaext_proxy_class *record, zend_class_entry *ce,
										   HashTable *allowlist)
{
	zval *entry;
	zend_function *constructor = NULL;

	ZEND_HASH_FOREACH_VAL(allowlist, entry)
	{
		zend_string *requested;
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

		/* Looked up in the class's own table, not resolved as a callable: a
		 * name the class does not declare must be an error, never a detour
		 * through __call. _lc because function_table keys are lowercase. */
		method = (zend_function *)zend_hash_str_find_ptr_lc(
			&ce->function_table, ZSTR_VAL(requested), ZSTR_LEN(requested));

		if (method == NULL) {
			zend_throw_exception_ex(luaext_ce_configuration_error, 0,
									"%s has no method %s() to expose to Lua", ZSTR_VAL(ce->name),
									ZSTR_VAL(requested));
			return false;
		}

		if (method == ce->constructor) {
			constructor = method;
			continue;
		}

		if (ce->__tostring != NULL && method == ce->__tostring) {
			record->to_string = method;
			continue;
		}

		refusal = luaext_proxy_method_refusal(method);

		if (refusal != NULL) {
			zend_throw_exception_ex(luaext_ce_configuration_error, 0,
									"%s::%s() cannot be exposed to Lua: %s", ZSTR_VAL(ce->name),
									ZSTR_VAL(requested), refusal);
			return false;
		}

		if (method->common.fn_flags & ZEND_ACC_STATIC) {
			if (!luaext_proxy_table_add(record->static_methods, ce, requested, method)) {
				return false;
			}
		} else {
			if (!luaext_proxy_table_add(record->instance_methods, ce, requested, method)) {
				return false;
			}
		}
	}
	ZEND_HASH_FOREACH_END();

	if (constructor != NULL) {
		zend_string *name = ZSTR_INIT_LITERAL("new", 0);
		bool added = luaext_proxy_set_constructor(record, ce, constructor, name);

		zend_string_release(name);

		if (!added) {
			return false;
		}
	}

	return true;
}

/* -------------------------------------------------------------------------
 * The proxy userdata
 *
 * One metatable per registered class, built on first push and interned twice
 * in the registry: class record -> metatable (for pushing) and, inside the
 * luaext_key_proxymts map, metatable -> class record (for the identity test).
 * A metatable *is* its __gc, so each map entry vouches that a userdata
 * carrying that metatable is a luaext_proxy_ud — the same one-key-one-payload
 * rule luaext_phpcall_metatable() documents.
 * ---------------------------------------------------------------------- */

/*
 * The __gc finaliser. Runs inside the collector, so the PHP reference is
 * handed to the defer queue rather than released here — releasing could run
 * an arbitrary __destruct against the very state being swept. See
 * luaext_defer.h; the timing of the release is all that is given up.
 */
static int luaext_proxy_release(lua_State *L)
{
	luaext_proxy_ud *slot = (luaext_proxy_ud *)lua_touserdata(L, 1);
	luaext_sandbox *sandbox = LUAEXT_SB(L);
	zval carrier;

	if (slot == NULL || slot->magic != LUAEXT_PROXY_MAGIC) {
		return 0;
	}

	/* A finalised proxy is no longer one of ours, even if it is resurrected. */
	slot->magic = 0;

	/* Swap-remove from the live list, keeping the moved entry's index true. */
	if (sandbox != NULL && slot->gc_index < sandbox->proxy_gc_count &&
		sandbox->proxy_gc_items[slot->gc_index] == slot) {
		size_t last = --sandbox->proxy_gc_count;

		sandbox->proxy_gc_items[slot->gc_index] = sandbox->proxy_gc_items[last];
		sandbox->proxy_gc_items[slot->gc_index]->gc_index = slot->gc_index;
	}

	if (slot->object != NULL) {
		ZVAL_OBJ(&carrier, slot->object);
		slot->object = NULL;

		if (sandbox == NULL || !luaext_defer_zval(sandbox, &carrier)) {
			/* Releasing here risks the narrow re-entrancy window; leaking
			 * would be worse, and a failed queue growth means the process is
			 * already out of memory. */
			zval_ptr_dtor(&carrier);
		}
	}

	return 0;
}

/*
 * Instance dispatch. Upvalues: (1) the class record, (2) the vetted
 * zend_function, (3) the Lua-visible name. Colon convention: the first
 * argument must be a proxy of this exact class — subclass instances already
 * wrapped as their nearest registered ancestor, so one identity check covers
 * the hierarchy. Everything past self converts through the shared boundary,
 * which is what makes chaining work: a returned registered instance wraps on
 * the way back out.
 */
static int luaext_proxy_method_call(lua_State *L)
{
	luaext_proxy_class *cls = (luaext_proxy_class *)lua_touserdata(L, lua_upvalueindex(1));
	zend_function *method = (zend_function *)lua_touserdata(L, lua_upvalueindex(2));
	const char *name = lua_tostring(L, lua_upvalueindex(3));
	luaext_proxy_ud *self = luaext_proxy_test(LUAEXT_SB(L), L, 1);
	luaext_phpcall_target target;

	if (self == NULL || self->cls != cls) {
		luaext_error_raise(L, LUAEXT_ERR_RUNTIME, false,
						   "method '%s' must be called with a colon (obj:%s(...))", name, name);
	}

	target.fcc = NULL;
	target.fn = method;
	target.bound = self->object;
	target.scope = self->object->ce;
	target.label = name;
	target.first_arg = 2;

	return luaext_phpcall_invoke_target(L, &target);
}

/*
 * Static dispatch. Upvalues: (1) the class record, (2) the vetted
 * zend_function, (3) the Lua-visible name, (4) the class table itself — the
 * colon guard: `money:zero()` desugars to `money.zero(money)`, so a first
 * argument that IS the table is that mistake, named with its fix.
 */
static int luaext_proxy_static_call(lua_State *L)
{
	luaext_proxy_class *cls = (luaext_proxy_class *)lua_touserdata(L, lua_upvalueindex(1));
	zend_function *method = (zend_function *)lua_touserdata(L, lua_upvalueindex(2));
	const char *name = lua_tostring(L, lua_upvalueindex(3));
	luaext_phpcall_target target;

	if (lua_gettop(L) >= 1 && lua_rawequal(L, 1, lua_upvalueindex(4))) {
		luaext_error_raise(L, LUAEXT_ERR_RUNTIME, false,
						   "static '%s' is called with a dot (%s.%s(...))", name,
						   ZSTR_VAL(cls->lua_name), name);
	}

	target.fcc = NULL;
	target.fn = method;
	target.bound = NULL;
	target.scope = cls->ce;
	target.label = name;
	target.first_arg = 1;

	return luaext_phpcall_invoke_target(L, &target);
}

/*
 * The exposed constructor, published as `.new` (or its override).
 *
 * The proxy is pushed BEFORE the constructor runs, deliberately: from that
 * moment the new object is Lua-owned, so this frame owns nothing across the
 * boundary call and a constructor that throws raises with nothing to leak —
 * the discarded proxy releases the half-constructed object through the
 * ordinary __gc/defer path. (The alternative — holding the object in a local
 * zval across a call that can raise — is exactly the leak the NO_RAISE
 * discipline exists to forbid.)
 */
static int luaext_proxy_new_call(lua_State *L)
{
	luaext_proxy_class *cls = (luaext_proxy_class *)lua_touserdata(L, lua_upvalueindex(1));
	luaext_sandbox *sandbox = LUAEXT_SB(L);
	luaext_phpcall_target target;
	zend_object *object;
	zval instance;

	if (sandbox == NULL || sandbox->closed || sandbox->L == NULL) {
		luaext_error_raise(L, LUAEXT_ERR_ABORT, true, "%s.%s cannot run: its sandbox is gone",
						   ZSTR_VAL(cls->lua_name), ZSTR_VAL(cls->constructor_lua_name));
	}

	if (object_init_ex(&instance, cls->ce) == FAILURE || EG(exception) != NULL) {
		luaext_error_raise_from_exception(L);
	}

	object = Z_OBJ(instance);

	if (!luaext_proxy_try_push(sandbox, L, object)) {
		/* Unreachable while registration implies findability; refuse loudly
		 * rather than hand the script a raw refusal about its own class. */
		zval_ptr_dtor(&instance);
		luaext_error_raise(L, LUAEXT_ERR_ABORT, true, "%s.%s could not wrap its own instance",
						   ZSTR_VAL(cls->lua_name), ZSTR_VAL(cls->constructor_lua_name));
	}

	/* Lua owns it now; the local reference goes before anything can raise. */
	zval_ptr_dtor(&instance);

	/* Stack: [arg1..argN, proxy] -> [proxy, arg1..argN], so the arguments sit
	 * above first_arg and the proxy survives the call as slot 1. */
	lua_insert(L, 1);

	target.fcc = NULL;
	target.fn = cls->constructor;
	target.bound = object;
	target.scope = cls->ce;
	target.label = ZSTR_VAL(cls->constructor_lua_name);
	target.first_arg = 2;

	(void)luaext_phpcall_invoke_target(L, &target);

	/* Drop the constructor's nil result; the proxy is the value of `.new`. */
	lua_settop(L, 1);

	return 1;
}

/*
 * The __eq metamethod, on every proxy metatable: an equality that tells the
 * truth and can never abort a script. Lua selects __eq for ANY userdata pair
 * (and, under debugMutate, for tables wearing a stolen metatable), so the
 * chain runs in a fixed order, each step making the next sound:
 *
 *   1. both operands are this sandbox's live proxies — else false, without
 *      ever dereferencing foreign memory (luaext_proxy_test guarantees it);
 *   2. pointer-equal objects — true, no PHP call: the same object is always
 *      equal to itself, and `a == a` stays free;
 *   3. proxies of different registered classes — false, no PHP call, which
 *      also keeps `a == b` and `b == a` in agreement across classes;
 *   4. a mapped Equality method, when the class configured one — else false:
 *      the default is pointer semantics.
 */
static int luaext_proxy_eq(lua_State *L)
{
	luaext_sandbox *sandbox = LUAEXT_SB(L);
	luaext_proxy_ud *left = luaext_proxy_test(sandbox, L, 1);
	luaext_proxy_ud *right = luaext_proxy_test(sandbox, L, 2);
	luaext_phpcall_target target;

	if (left == NULL || right == NULL) {
		lua_pushboolean(L, 0);
		return 1;
	}

	if (left->object == right->object) {
		lua_pushboolean(L, 1);
		return 1;
	}

	if (left->cls != right->cls || left->cls->op_methods[LUAEXT_PROXY_OP_EQ] == NULL) {
		lua_pushboolean(L, 0);
		return 1;
	}

	target.fcc = NULL;
	target.fn = left->cls->op_methods[LUAEXT_PROXY_OP_EQ];
	target.bound = left->object;
	target.scope = left->object->ce;
	target.label = "==";

	/* Keep both operands anchored; the right one converts from a pushed copy
	 * so a mid-call GC can never finalise an operand still in use. */
	lua_settop(L, 2);
	lua_pushvalue(L, 2);
	target.first_arg = 3;

	return luaext_phpcall_invoke_target(L, &target);
}

/* The __tostring metamethod, present only when __toString() was marked. */
static int luaext_proxy_tostring(lua_State *L)
{
	luaext_proxy_class *cls = (luaext_proxy_class *)lua_touserdata(L, lua_upvalueindex(1));
	luaext_proxy_ud *self = luaext_proxy_test(LUAEXT_SB(L), L, 1);
	luaext_phpcall_target target;

	if (self == NULL || self->cls != cls) {
		luaext_error_raise(L, LUAEXT_ERR_RUNTIME, false,
						   "tostring() received a value that is not a %s proxy",
						   ZSTR_VAL(cls->lua_name));
	}

	/* Lua calls __tostring with just the value; drop anything above it so the
	 * boundary converts no stray arguments. */
	lua_settop(L, 1);

	target.fcc = NULL;
	target.fn = cls->to_string;
	target.bound = self->object;
	target.scope = self->object->ce;
	target.label = "__toString";
	target.first_arg = 2;

	return luaext_phpcall_invoke_target(L, &target);
}

/*
 * The allocating half of planting a class table, run under lua_pcall: every
 * step can raise on a memory error, and the caller is a PHP method body where
 * a raise has nothing to unwind to. Argument 1: the class record.
 */
static int luaext_proxy_plant_table(lua_State *L)
{
	luaext_proxy_class *cls = (luaext_proxy_class *)lua_touserdata(L, 1);
	zend_string *name;
	zend_function *method;

	lua_settop(L, 0);
	luaL_checkstack(L, 8, "luaext: no stack to build a class table");

	lua_createtable(L, 0, (int)zend_hash_num_elements(cls->static_methods) + 1);

	ZEND_HASH_MAP_FOREACH_STR_KEY_PTR(cls->static_methods, name, method)
	{
		lua_pushlightuserdata(L, cls);
		lua_pushlightuserdata(L, method);
		lua_pushlstring(L, ZSTR_VAL(name), ZSTR_LEN(name));
		lua_pushvalue(L, 1); /* the table itself, for the colon guard */
		lua_pushcclosure(L, luaext_proxy_static_call, 4);
		lua_setfield(L, 1, ZSTR_VAL(name));
	}
	ZEND_HASH_FOREACH_END();

	if (cls->constructor != NULL) {
		lua_pushlightuserdata(L, cls);
		lua_pushcclosure(L, luaext_proxy_new_call, 1);
		lua_setfield(L, 1, ZSTR_VAL(cls->constructor_lua_name));
	}

	lua_setglobal(L, ZSTR_VAL(cls->lua_name));

	return 0;
}

/* Push the proxymts map (metatable -> class light ud), creating it lazily. */
static void luaext_proxy_mts_map(lua_State *L)
{
	if (lua_rawgetp(L, LUA_REGISTRYINDEX, &luaext_key_proxymts) == LUA_TTABLE) {
		return;
	}

	lua_pop(L, 1);
	lua_createtable(L, 0, 2);
	lua_pushvalue(L, -1);
	lua_rawsetp(L, LUA_REGISTRYINDEX, &luaext_key_proxymts);
}

/* Push `cls`'s metatable, building and interning it on first use. */
static void luaext_proxy_push_metatable(lua_State *L, luaext_proxy_class *cls)
{
	if (lua_rawgetp(L, LUA_REGISTRYINDEX, cls) == LUA_TTABLE) {
		return;
	}

	lua_pop(L, 1);
	lua_createtable(L, 0, 4);

	lua_pushcfunction(L, luaext_proxy_release);
	lua_setfield(L, -2, "__gc");

	/* Locked for the same reason every subsystem locks it: a script that
	 * could read this table back out could replace __gc, and one that could
	 * stamp it onto its own value could hand the collector a forged payload. */
	lua_pushboolean(L, 0);
	lua_setfield(L, -2, "__metatable");

	lua_pushcfunction(L, luaext_proxy_eq);
	lua_setfield(L, -2, "__eq");

	/* __index: one shared closure per exposed instance method. A missing
	 * name reads as nil and fails the standard Lua way. */
	{
		zend_string *method_name;
		zend_function *method;

		lua_createtable(L, 0, (int)zend_hash_num_elements(cls->instance_methods));

		ZEND_HASH_MAP_FOREACH_STR_KEY_PTR(cls->instance_methods, method_name, method)
		{
			lua_pushlightuserdata(L, cls);
			lua_pushlightuserdata(L, method);
			lua_pushlstring(L, ZSTR_VAL(method_name), ZSTR_LEN(method_name));
			lua_pushcclosure(L, luaext_proxy_method_call, 3);
			lua_setfield(L, -2, ZSTR_VAL(method_name));
		}
		ZEND_HASH_FOREACH_END();

		lua_setfield(L, -2, "__index");
	}

	if (cls->to_string != NULL) {
		lua_pushlightuserdata(L, cls);
		lua_pushcclosure(L, luaext_proxy_tostring, 1);
		lua_setfield(L, -2, "__tostring");
	}

	lua_pushvalue(L, -1);
	lua_rawsetp(L, LUA_REGISTRYINDEX, cls);

	luaext_proxy_mts_map(L);
	lua_pushvalue(L, -2);
	lua_pushlightuserdata(L, cls);
	lua_rawset(L, -3);
	lua_pop(L, 1);
}

bool luaext_proxy_try_push(luaext_sandbox *sandbox, lua_State *L, zend_object *object)
{
	luaext_proxy_class *cls = luaext_proxy_find(sandbox, object->ce);
	luaext_proxy_ud *slot;

	if (cls == NULL) {
		return false;
	}

	/* May raise on memory pressure; nothing is owned before this returns. */
	slot = (luaext_proxy_ud *)lua_newuserdatauv(L, sizeof(*slot), 0);
	memset(slot, 0, sizeof(*slot));

	/* Armed while magic is still zero, so a raise below leaves a userdata
	 * whose finaliser no-ops rather than one holding an unreleased ref. */
	luaext_proxy_push_metatable(L, cls);
	lua_setmetatable(L, -2);

	if (sandbox->proxy_gc_count == sandbox->proxy_gc_cap) {
		size_t cap = sandbox->proxy_gc_cap == 0 ? 8 : sandbox->proxy_gc_cap * 2;

		sandbox->proxy_gc_items = (luaext_proxy_ud **)perealloc(
			sandbox->proxy_gc_items, cap * sizeof(*sandbox->proxy_gc_items), 1);
		sandbox->proxy_gc_cap = cap;
	}

	slot->object = object;
	GC_ADDREF(object);
	slot->cls = cls;
	slot->gc_index = sandbox->proxy_gc_count;
	sandbox->proxy_gc_items[sandbox->proxy_gc_count++] = slot;

	/* Last: only a fully-listed, reference-holding payload is a live proxy. */
	slot->magic = LUAEXT_PROXY_MAGIC;

	return true;
}

luaext_proxy_ud *luaext_proxy_test(luaext_sandbox *sandbox, lua_State *L, int index)
{
	luaext_proxy_ud *slot;
	bool ours;

	(void)sandbox;

	index = lua_absindex(L, index);

	/*
	 * The four gates, in order, each making the next read sound. The first
	 * two exist because metamethods and dispatch closures receive arbitrary
	 * values — under debugMutate a script can stamp a genuine proxy metatable
	 * onto a plain table or a foreign userdata, and nothing may be read out
	 * of the payload until the value is provably one of ours.
	 */
	if (lua_type(L, index) != LUA_TUSERDATA) {
		return NULL;
	}

	if (lua_rawlen(L, index) != sizeof(luaext_proxy_ud)) {
		return NULL;
	}

	if (!lua_getmetatable(L, index)) {
		return NULL;
	}

	if (lua_rawgetp(L, LUA_REGISTRYINDEX, &luaext_key_proxymts) != LUA_TTABLE) {
		lua_pop(L, 2);
		return NULL;
	}

	lua_pushvalue(L, -2);
	lua_rawget(L, -2);
	ours = lua_type(L, -1) == LUA_TLIGHTUSERDATA;
	lua_pop(L, 3);

	if (!ours) {
		return NULL;
	}

	slot = (luaext_proxy_ud *)lua_touserdata(L, index);

	if (slot == NULL || slot->magic != LUAEXT_PROXY_MAGIC || slot->object == NULL) {
		return NULL;
	}

	return slot;
}

void luaext_proxy_add_gc(const luaext_sandbox *sandbox, zend_get_gc_buffer *buffer)
{
	size_t index;

	for (index = 0; index < sandbox->proxy_gc_count; index++) {
		zend_get_gc_buffer_add_obj(buffer, sandbox->proxy_gc_items[index]->object);
	}
}

/* -------------------------------------------------------------------------
 * The registry
 * ---------------------------------------------------------------------- */

static void luaext_proxy_class_free(luaext_proxy_class *record)
{
	if (record->instance_methods != NULL) {
		zend_hash_destroy(record->instance_methods);
		pefree(record->instance_methods, 1);
	}

	if (record->static_methods != NULL) {
		zend_hash_destroy(record->static_methods);
		pefree(record->static_methods, 1);
	}

	if (record->lua_name != NULL) {
		zend_string_release(record->lua_name);
	}

	if (record->constructor_lua_name != NULL) {
		zend_string_release(record->constructor_lua_name);
	}

	pefree(record, 1);
}

bool luaext_proxy_register(luaext_sandbox *sandbox, zend_string *class_name, HashTable *allowlist,
						   zend_string *lua_name, HashTable *operators)
{
	zend_class_entry *ce;
	luaext_proxy_class *walk;
	luaext_proxy_class *record;
	zend_string *resolved_name;
	bool collected;

	ce = zend_lookup_class(class_name);

	if (ce == NULL) {
		/* An autoloader that threw already explains the failure better. */
		if (EG(exception) == NULL) {
			zend_throw_exception_ex(luaext_ce_configuration_error, 0,
									"Cannot register %s: the class does not exist",
									ZSTR_VAL(class_name));
		}

		return false;
	}

	if (ce->ce_flags & ZEND_ACC_INTERFACE) {
		zend_throw_exception_ex(
			luaext_ce_configuration_error, 0,
			"Cannot register interface %s: only classes have instances to proxy",
			ZSTR_VAL(ce->name));
		return false;
	}

	if (ce->ce_flags & ZEND_ACC_ENUM) {
		zend_throw_exception_ex(luaext_ce_configuration_error, 0,
								"Cannot register enum %s: enum cases are process-lifetime "
								"singletons, not instances a proxy can own",
								ZSTR_VAL(ce->name));
		return false;
	}

	if (ce->ce_flags & ZEND_ACC_ANON_CLASS) {
		zend_throw_exception(luaext_ce_configuration_error,
							 "Cannot register an anonymous class: it has no usable Lua name", 0);
		return false;
	}

	for (walk = sandbox->proxy_classes; walk != NULL; walk = walk->next) {
		if (walk->ce == ce) {
			zend_throw_exception_ex(luaext_ce_configuration_error, 0,
									"%s is already registered on this sandbox", ZSTR_VAL(ce->name));
			return false;
		}
	}

	/*
	 * The Lua name: parameter, else the unqualified class name. Recorded even
	 * for a class that plants no table, so name collisions are refused by one
	 * rule regardless of what a later registration exposes.
	 */
	if (lua_name != NULL) {
		if (ZSTR_LEN(lua_name) == 0 ||
			memchr(ZSTR_VAL(lua_name), '\0', ZSTR_LEN(lua_name)) != NULL) {
			zend_throw_exception_ex(luaext_ce_configuration_error, 0,
									"The Lua name for %s must be a non-empty string without NUL "
									"bytes",
									ZSTR_VAL(ce->name));
			return false;
		}

		resolved_name = luaext_proxy_pstr(lua_name);
	} else {
		const char *backslash =
			(const char *)zend_memrchr(ZSTR_VAL(ce->name), '\\', ZSTR_LEN(ce->name));
		const char *short_name = backslash != NULL ? backslash + 1 : ZSTR_VAL(ce->name);

		resolved_name = zend_string_init(
			short_name, ZSTR_LEN(ce->name) - (size_t)(short_name - ZSTR_VAL(ce->name)), 1);
	}

	for (walk = sandbox->proxy_classes; walk != NULL; walk = walk->next) {
		if (zend_string_equals(walk->lua_name, resolved_name)) {
			zend_string_release(resolved_name);
			zend_throw_exception_ex(
				luaext_ce_configuration_error, 0,
				"The Lua name \"%s\" is already taken by a previously registered class",
				lua_name != NULL ? ZSTR_VAL(lua_name) : ZSTR_VAL(ce->name));
			return false;
		}
	}

	record = pecalloc(1, sizeof(*record), 1);
	record->ce = ce;
	record->lua_name = resolved_name;
	record->instance_methods = pemalloc(sizeof(HashTable), 1);
	zend_hash_init(record->instance_methods, 8, NULL, NULL, 1);
	record->static_methods = pemalloc(sizeof(HashTable), 1);
	zend_hash_init(record->static_methods, 8, NULL, NULL, 1);

	if (allowlist != NULL) {
		collected = luaext_proxy_collect_allowlist(record, ce, allowlist);
	} else {
		collected = luaext_proxy_collect_attributed(record, ce);
	}

	/*
	 * Instances of an abstract class cannot exist, so an exposed constructor
	 * on one is a mistake stored — and the class may still register for
	 * proxying, since concrete subclasses wrap as their nearest registered
	 * ancestor.
	 */
	if (collected && record->constructor != NULL &&
		(ce->ce_flags & ZEND_ACC_EXPLICIT_ABSTRACT_CLASS)) {
		zend_throw_exception_ex(luaext_ce_configuration_error, 0,
								"Cannot expose the constructor of abstract %s", ZSTR_VAL(ce->name));
		collected = false;
	}

	/*
	 * Shape check only: keys are method names, values are Operator cases.
	 * Full slot validation (arity, bool returns, duplicates) is the operator
	 * subsystem's, which also fills op_methods.
	 */
	if (collected && operators != NULL) {
		zend_string *op_key;
		zval *op_value;

		ZEND_HASH_FOREACH_STR_KEY_VAL(operators, op_key, op_value)
		{
			ZVAL_DEREF(op_value);

			if (op_key == NULL || ZSTR_LEN(op_key) == 0 || Z_TYPE_P(op_value) != IS_OBJECT ||
				!instanceof_function(Z_OBJCE_P(op_value), luaext_ce_operator)) {
				zend_throw_exception_ex(
					luaext_ce_configuration_error, 0,
					"The operator map for %s must map method names to Operator cases",
					ZSTR_VAL(ce->name));
				collected = false;
				break;
			}
		}
		ZEND_HASH_FOREACH_END();
	}

	if (collected && zend_hash_num_elements(record->instance_methods) == 0 &&
		zend_hash_num_elements(record->static_methods) == 0 && record->constructor == NULL &&
		record->to_string == NULL &&
		(operators == NULL || zend_hash_num_elements(operators) == 0)) {
		zend_throw_exception_ex(luaext_ce_configuration_error, 0,
								"Nothing of %s is exposed: no method carries #[LuaMethod], no "
								"allowlist was given, and no operator is mapped",
								ZSTR_VAL(ce->name));
		collected = false;
	}

	if (!collected) {
		luaext_proxy_class_free(record);
		return false;
	}

	/*
	 * Plant the class table before the record is linked, so a refusal here is
	 * atomic: on failure the record (and the dead closures inside the
	 * never-published table, which reference it but can never run) is freed
	 * and the script's view of the world is untouched. A class exposing only
	 * instance methods plants nothing and simply becomes eligible to cross.
	 */
	if (zend_hash_num_elements(record->static_methods) > 0 || record->constructor != NULL) {
		lua_State *L = sandbox->running_L != NULL ? sandbox->running_L : sandbox->L;
		int status;

		lua_pushcfunction(L, luaext_proxy_plant_table);
		lua_pushlightuserdata(L, record);
		status = lua_pcall(L, 1, 0, 0);

		if (status != LUA_OK) {
			luaext_error_throw_from_lua(sandbox, L, status);
			luaext_proxy_class_free(record);
			return false;
		}
	}

	record->next = sandbox->proxy_classes;
	sandbox->proxy_classes = record;

	return true;
}

luaext_proxy_class *luaext_proxy_find(const luaext_sandbox *sandbox, const zend_class_entry *ce)
{
	const zend_class_entry *ancestor;

	/*
	 * Nearest registered ancestor: the instance's own class first, then each
	 * parent in turn. Interfaces are never consulted — "nearest" has no
	 * meaning across a lattice.
	 */
	for (ancestor = ce; ancestor != NULL; ancestor = ancestor->parent) {
		luaext_proxy_class *record;

		for (record = sandbox->proxy_classes; record != NULL; record = record->next) {
			if (record->ce == ancestor) {
				return record;
			}
		}
	}

	return NULL;
}

void luaext_proxy_shutdown(luaext_sandbox *sandbox)
{
	luaext_proxy_class *record = sandbox->proxy_classes;

	sandbox->proxy_classes = NULL;

	while (record != NULL) {
		luaext_proxy_class *next = record->next;

		luaext_proxy_class_free(record);
		record = next;
	}

	/* lua_close() finalised every proxy, so the list is empty by now; the
	 * storage is what remains. */
	if (sandbox->proxy_gc_items != NULL) {
		pefree(sandbox->proxy_gc_items, 1);
		sandbox->proxy_gc_items = NULL;
	}

	sandbox->proxy_gc_count = 0;
	sandbox->proxy_gc_cap = 0;
}
