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
#include "luaext_sandbox.h"

#include <lauxlib.h>
#include <lua.h>

#include <string.h>

#include <Zend/zend_attributes.h>
#include <Zend/zend_enum.h>
#include <Zend/zend_exceptions.h>

/* -------------------------------------------------------------------------
 * Operator slots
 *
 * The single source of truth tying the PHP Operator enum, Lua's metamethod
 * names, and validation rules together. EQ carries no metamethod name: value
 * equality is a step inside the __eq chain, never a separate handler.
 * ---------------------------------------------------------------------- */

static const struct {
	const char *case_name;	/* the PHP enum case */
	const char *metamethod; /* Lua event key; NULL for EQ */
	const char *symbol;		/* how messages spell the operator */
	bool comparison;		/* one required parameter + declared bool return */
	bool unary;				/* no required parameters */
} luaext_proxy_ops[LUAEXT_PROXY_OP__COUNT] = {
	[LUAEXT_PROXY_OP_LT] = {"LessThan", "__lt", "<", true, false},
	[LUAEXT_PROXY_OP_LE] = {"LessThanOrEqual", "__le", "<=", true, false},
	[LUAEXT_PROXY_OP_EQ] = {"Equality", NULL, "==", true, false},
	[LUAEXT_PROXY_OP_ADD] = {"Add", "__add", "+", false, false},
	[LUAEXT_PROXY_OP_SUB] = {"Subtract", "__sub", "-", false, false},
	[LUAEXT_PROXY_OP_MUL] = {"Multiply", "__mul", "*", false, false},
	[LUAEXT_PROXY_OP_DIV] = {"Divide", "__div", "/", false, false},
	[LUAEXT_PROXY_OP_MOD] = {"Modulo", "__mod", "%", false, false},
	[LUAEXT_PROXY_OP_POW] = {"Power", "__pow", "^", false, false},
	[LUAEXT_PROXY_OP_UNM] = {"UnaryMinus", "__unm", "-", false, true},
	[LUAEXT_PROXY_OP_CONCAT] = {"Concatenate", "__concat", "..", false, false},
};

/* -------------------------------------------------------------------------
 * Small helpers
 * ---------------------------------------------------------------------- */

/* The two halves of pushing a proxy; defined beside luaext_proxy_try_push. */
static luaext_proxy_ud *luaext_proxy_push_shell(luaext_sandbox *sandbox, lua_State *L,
												luaext_proxy_class *cls);
static void luaext_proxy_bind(luaext_sandbox *sandbox, luaext_proxy_ud *slot,
							  luaext_proxy_class *cls, zend_object *object);

/* Retired-record reclamation and record teardown; defined further down. */
static void luaext_proxy_discard(luaext_sandbox *sandbox, lua_State *L, luaext_proxy_class *record);
static void luaext_proxy_class_free(luaext_proxy_class *record);

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
	const zend_string *method_name = method->common.function_name;

	if (!(method->common.fn_flags & ZEND_ACC_PUBLIC)) {
		return "it is not public";
	}

	if (method->common.fn_flags & ZEND_ACC_ABSTRACT) {
		return "it is abstract";
	}

	/*
	 * Magic methods are refused for the same reason the phpcall boundary
	 * refuses them wholesale: exposing __call would turn one selection into
	 * every name the class can be asked for. The two the collectors route to
	 * dedicated slots — __construct becomes .new, __toString becomes the
	 * metamethod — are the deliberate exceptions, vetted here like any other.
	 */
	if (method_name != NULL && ZSTR_LEN(method_name) >= 2 && ZSTR_VAL(method_name)[0] == '_' &&
		ZSTR_VAL(method_name)[1] == '_' &&
		!zend_string_equals_literal_ci(method_name, "__construct") &&
		!zend_string_equals_literal_ci(method_name, "__toString")) {
		return "it is a magic method, and magic methods are never exposed";
	}

	return NULL;
}

/*
 * The one shape rule for every name published on a class table or metatable:
 * the interpreter half writes C strings, so a name that smuggles a NUL would
 * be published truncated while every uniqueness check saw the full bytes.
 */
static bool luaext_proxy_name_publishable(const zend_class_entry *ce, const zend_string *lua_name)
{
	if (ZSTR_LEN(lua_name) == 0 || memchr(ZSTR_VAL(lua_name), '\0', ZSTR_LEN(lua_name)) != NULL) {
		zend_throw_exception_ex(
			luaext_ce_configuration_error, 0,
			"A Lua method name on %s must be a non-empty string without NUL bytes",
			ZSTR_VAL(ce->name));
		return false;
	}

	return true;
}

/*
 * Add one selected method under its Lua name, refusing a duplicate key. The
 * instance and static tables are separate namespaces (metatable __index vs
 * the class table), so only same-table duplicates are collisions.
 */
static bool luaext_proxy_table_add(HashTable *table, const zend_class_entry *ce,
								   zend_string *lua_name, zend_function *method)
{
	zend_string *key;

	if (!luaext_proxy_name_publishable(ce, lua_name)) {
		return false;
	}

	key = luaext_proxy_pstr(lua_name);

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
	if (!luaext_proxy_name_publishable(ce, name)) {
		return false;
	}

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

		/* Eligibility before routing, matching the attribute route: the
		 * constructor and __toString earn their dedicated slots only when
		 * they would have been exposable as ordinary methods. */
		refusal = luaext_proxy_method_refusal(method);

		if (refusal != NULL) {
			zend_throw_exception_ex(luaext_ce_configuration_error, 0,
									"%s::%s() cannot be exposed to Lua: %s", ZSTR_VAL(ce->name),
									ZSTR_VAL(requested), refusal);
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

	/* The last proxy of a retired class takes the record with it; both the
	 * scrub and the free are pure C work, safe inside a finaliser. */
	if (slot->cls != NULL) {
		luaext_proxy_class *cls = slot->cls;

		slot->cls = NULL;

		if (cls->live_proxies > 0) {
			cls->live_proxies--;
		}

		if (cls->retired && cls->live_proxies == 0 && sandbox != NULL) {
			luaext_proxy_discard(sandbox, L, cls);
		}
	}

	return 0;
}

/*
 * The zend_function to actually call on `object`: the registered method, or
 * the object's own override of it. The record's pointer is the REGISTERED
 * class's implementation, and a subclass instance wrapping as its nearest
 * registered ancestor must still run its own overrides — exactly as a bound
 * callable from registerObject() would. PHP forbids narrowing visibility, so
 * an override of a vetted public method is itself public.
 */
static zend_function *luaext_proxy_resolve_method(zend_object *object,
												  const luaext_proxy_class *cls,
												  zend_function *method)
{
	zend_function *override;

	if (object->ce == cls->ce) {
		return method;
	}

	override = (zend_function *)zend_hash_str_find_ptr_lc(&object->ce->function_table,
														  ZSTR_VAL(method->common.function_name),
														  ZSTR_LEN(method->common.function_name));

	return override != NULL ? override : method;
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
	target.fn = luaext_proxy_resolve_method(self->object, cls, method);
	target.bound = self->object;
	target.scope = self->object->ce;
	target.label = name;
	target.first_arg = 2;

	return luaext_phpcall_invoke_target(L, &target);
}

/*
 * Static dispatch. Upvalues: (1) the class-table anchor, (2) the vetted
 * zend_function, (3) the Lua-visible name, (4) the class table itself — the
 * colon guard: `money:zero()` desugars to `money.zero(money)`, so a first
 * argument that IS the table is that mistake, named with its fix.
 */
static int luaext_proxy_static_call(lua_State *L)
{
	const luaext_proxy_anchor *anchor =
		(const luaext_proxy_anchor *)lua_touserdata(L, lua_upvalueindex(1));
	zend_function *method = (zend_function *)lua_touserdata(L, lua_upvalueindex(2));
	const char *name = lua_tostring(L, lua_upvalueindex(3));
	luaext_proxy_class *cls;
	luaext_phpcall_target target;

	/* Before any read of the record: retirement revoked the anchor, and the
	 * record a stale alias would name may already be reclaimed. */
	if (anchor->magic != LUAEXT_PROXY_ANCHOR_MAGIC) {
		luaext_error_raise(L, LUAEXT_ERR_RUNTIME, false,
						   "'%s' cannot run: its registration was withdrawn", name);
	}

	cls = anchor->cls;

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
 * The exposed constructor, published as `.new` (or its override). Upvalues:
 * (1) the class-table anchor, (2) the printable "Class.new" name.
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
	const luaext_proxy_anchor *anchor =
		(const luaext_proxy_anchor *)lua_touserdata(L, lua_upvalueindex(1));
	const char *name = lua_tostring(L, lua_upvalueindex(2));
	luaext_sandbox *sandbox = LUAEXT_SB(L);
	luaext_proxy_class *cls;
	luaext_phpcall_target target;
	luaext_proxy_ud *slot;
	zend_object *object;
	zval instance;

	/* Before any read of the record: retirement revoked the anchor, and the
	 * record a stale alias would name may already be reclaimed. */
	if (anchor->magic != LUAEXT_PROXY_ANCHOR_MAGIC) {
		luaext_error_raise(L, LUAEXT_ERR_RUNTIME, false,
						   "%s cannot run: its registration was withdrawn", name);
	}

	cls = anchor->cls;

	if (sandbox == NULL || sandbox->closed || sandbox->L == NULL) {
		luaext_error_raise(L, LUAEXT_ERR_ABORT, true, "%s cannot run: its sandbox is gone", name);
	}

	if (EG(exception) != NULL) {
		luaext_error_raise_from_exception(L);
	}

	/*
	 * Lua-side allocation FIRST, while this frame owns nothing: the shell's
	 * userdata, metatable and GC-list growth are every step of a push that
	 * can raise (a billed allocator hitting memoryBytes longjmps), and a
	 * raise past a freshly-created object's sole reference would leak it.
	 * Only once the shell exists is the PHP object created — object_init_ex()
	 * cannot longjmp, a FAILURE leaves the zval undef — and the two are
	 * married by the binding half, which cannot raise at all.
	 */
	slot = luaext_proxy_push_shell(sandbox, L, cls);

	if (object_init_ex(&instance, cls->ce) == FAILURE) {
		/* The blank shell is collected as any garbage; zero magic no-ops its
		 * finaliser. Nothing is owned. */
		luaext_error_raise_from_exception(L);
	}

	if (EG(exception) != NULL) {
		/* Init SUCCEEDED, so the zval holds a fresh object this raise would
		 * strand; the defer queue carries it to the next boundary drain. */
		if (!luaext_defer_zval(sandbox, &instance)) {
			zval_ptr_dtor(&instance);
		}

		luaext_error_raise_from_exception(L);
	}

	object = Z_OBJ(instance);
	luaext_proxy_bind(sandbox, slot, cls, object);

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

/* Operand description for an operator error: class name or Lua type name. */
static const char *luaext_proxy_operand_name(luaext_sandbox *sandbox, lua_State *L, int index)
{
	const luaext_proxy_ud *proxy = luaext_proxy_test(sandbox, L, index);

	return proxy != NULL ? ZSTR_VAL(proxy->cls->lua_name) : luaL_typename(L, index);
}

/*
 * Dispatch a binary metamethod: `method` (or the left operand's override of
 * it) on the left operand, with the right operand as its one argument.
 *
 * Keeps BOTH operands anchored on the stack: removing the left proxy's only
 * anchor would let a mid-call GC finalise it while its zend_object is in use
 * as the receiver — survivable only because the defer queue holds the
 * reference until a drain that cannot run while in_lua > 0, and that is
 * nothing to lean on. The right operand converts from a pushed copy instead;
 * slots 1-2 stay put.
 */
static int luaext_proxy_call_binary(lua_State *L, luaext_proxy_ud *left, zend_function *method,
									const char *label)
{
	luaext_phpcall_target target;

	target.fcc = NULL;
	target.fn = luaext_proxy_resolve_method(left->object, left->cls, method);
	target.bound = left->object;
	target.scope = left->object->ce;
	target.label = label;

	lua_settop(L, 2);
	lua_pushvalue(L, 2);
	target.first_arg = 3;

	return luaext_phpcall_invoke_target(L, &target);
}

/*
 * Binary operator dispatch. Upvalues: (1) the class record, (2) the slot.
 *
 * Both operands must be proxies of the same registered class — one identity
 * check, since subclasses already wrapped as their nearest registered
 * ancestor. Any other shape raises the operator's own catchable error; mixed
 * forms stay named method calls when the wrapper exposes one. Loosening this
 * later is backward-compatible; tightening never would be.
 */
static int luaext_proxy_binop(lua_State *L)
{
	luaext_proxy_class *cls = (luaext_proxy_class *)lua_touserdata(L, lua_upvalueindex(1));
	int slot = (int)lua_tointeger(L, lua_upvalueindex(2));
	luaext_sandbox *sandbox = LUAEXT_SB(L);
	luaext_proxy_ud *left = luaext_proxy_test(sandbox, L, 1);
	luaext_proxy_ud *right = luaext_proxy_test(sandbox, L, 2);

	if (left == NULL || right == NULL || left->cls != cls || right->cls != cls) {
		const char *first = luaext_proxy_operand_name(sandbox, L, 1);
		const char *second = luaext_proxy_operand_name(sandbox, L, 2);

		if (luaext_proxy_ops[slot].comparison) {
			luaext_error_raise(L, LUAEXT_ERR_RUNTIME, false, "cannot compare %s with %s", first,
							   second);
		}

		if (slot == LUAEXT_PROXY_OP_CONCAT) {
			luaext_error_raise(L, LUAEXT_ERR_RUNTIME, false, "cannot concatenate %s and %s", first,
							   second);
		}

		luaext_error_raise(L, LUAEXT_ERR_RUNTIME, false, "cannot apply '%s' to %s and %s",
						   luaext_proxy_ops[slot].symbol, first, second);
	}

	return luaext_proxy_call_binary(L, left, cls->op_methods[slot], luaext_proxy_ops[slot].symbol);
}

/* Unary minus. Upvalue: the class record. Lua passes the operand twice. */
static int luaext_proxy_unop(lua_State *L)
{
	luaext_proxy_class *cls = (luaext_proxy_class *)lua_touserdata(L, lua_upvalueindex(1));
	luaext_sandbox *sandbox = LUAEXT_SB(L);
	luaext_proxy_ud *self = luaext_proxy_test(sandbox, L, 1);
	luaext_phpcall_target target;

	if (self == NULL || self->cls != cls) {
		luaext_error_raise(L, LUAEXT_ERR_RUNTIME, false, "cannot apply unary '-' to %s",
						   luaext_proxy_operand_name(sandbox, L, 1));
	}

	lua_settop(L, 1);

	target.fcc = NULL;
	target.fn =
		luaext_proxy_resolve_method(self->object, cls, cls->op_methods[LUAEXT_PROXY_OP_UNM]);
	target.bound = self->object;
	target.scope = self->object->ce;
	target.label = "-";
	target.first_arg = 2;

	return luaext_phpcall_invoke_target(L, &target);
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

	return luaext_proxy_call_binary(L, left, left->cls->op_methods[LUAEXT_PROXY_OP_EQ], "==");
}

/*
 * The __tostring metamethod, present only when __toString() was marked.
 * Upvalues: (1) the class record, (2) the class's Lua name — the refusal
 * message reads the STRING, never the record: a forged value can reach this
 * closure after its record was reclaimed, and only a live proxy of this very
 * class proves the record is still there to dereference.
 */
static int luaext_proxy_tostring(lua_State *L)
{
	luaext_proxy_class *cls = (luaext_proxy_class *)lua_touserdata(L, lua_upvalueindex(1));
	luaext_proxy_ud *self = luaext_proxy_test(LUAEXT_SB(L), L, 1);
	luaext_phpcall_target target;

	if (self == NULL || self->cls != cls) {
		luaext_error_raise(L, LUAEXT_ERR_RUNTIME, false,
						   "tostring() received a value that is not a %s proxy",
						   lua_tostring(L, lua_upvalueindex(2)));
	}

	/* Lua calls __tostring with just the value; drop anything above it so the
	 * boundary converts no stray arguments. */
	lua_settop(L, 1);

	target.fcc = NULL;
	/* The actual class's __toString, when it overrode the registered one. */
	target.fn =
		self->object->ce->__tostring != NULL ? self->object->ce->__tostring : cls->to_string;
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
	luaext_proxy_anchor *anchor;
	zend_string *name;
	zend_function *method;

	lua_settop(L, 0);
	luaL_checkstack(L, 8, "luaext: no stack to build a class table");

	lua_createtable(L, 0, (int)zend_hash_num_elements(cls->static_methods) + 1);

	/*
	 * The validity token every dispatch closure captures instead of the record
	 * pointer, so a stale alias refuses instead of reaching reclaimed memory.
	 * Committed to the record immediately: a raise below leaves the caller to
	 * revoke it alongside freeing the record it guards.
	 */
	anchor = (luaext_proxy_anchor *)lua_newuserdatauv(L, sizeof(*anchor), 0);
	anchor->magic = LUAEXT_PROXY_ANCHOR_MAGIC;
	anchor->cls = cls;
	lua_pushvalue(L, 2);
	cls->anchor_ref = luaL_ref(L, LUA_REGISTRYINDEX);
	cls->anchor = anchor;

	ZEND_HASH_MAP_FOREACH_STR_KEY_PTR(cls->static_methods, name, method)
	{
		lua_pushvalue(L, 2); /* the anchor */
		lua_pushlightuserdata(L, method);
		lua_pushlstring(L, ZSTR_VAL(name), ZSTR_LEN(name));
		lua_pushvalue(L, 1); /* the table itself, for the colon guard */
		lua_pushcclosure(L, luaext_proxy_static_call, 4);
		lua_setfield(L, 1, ZSTR_VAL(name));
	}
	ZEND_HASH_FOREACH_END();

	if (cls->constructor != NULL) {
		lua_pushvalue(L, 2); /* the anchor */
		lua_pushfstring(L, "%s.%s", ZSTR_VAL(cls->lua_name), ZSTR_VAL(cls->constructor_lua_name));
		lua_pushcclosure(L, luaext_proxy_new_call, 2);
		lua_setfield(L, 1, ZSTR_VAL(cls->constructor_lua_name));
	}

	lua_pop(L, 1); /* the anchor; the pin and the closures hold it now */

	/* Raw, like every host-side write into the globals table: a script-
	 * installed _G metamethod must not run unmetered inside a host call. */
	lua_pushglobaltable(L);
	lua_pushlstring(L, ZSTR_VAL(cls->lua_name), ZSTR_LEN(cls->lua_name));
	lua_pushvalue(L, 1);
	lua_rawset(L, 2);

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
		lua_pushlstring(L, ZSTR_VAL(cls->lua_name), ZSTR_LEN(cls->lua_name));
		lua_pushcclosure(L, luaext_proxy_tostring, 2);
		lua_setfield(L, -2, "__tostring");
	}

	/* A slot's metamethod exists only when mapped; EQ lives inside __eq. */
	{
		int slot;

		for (slot = 0; slot < LUAEXT_PROXY_OP__COUNT; slot++) {
			if (cls->op_methods[slot] == NULL || luaext_proxy_ops[slot].metamethod == NULL) {
				continue;
			}

			lua_pushlightuserdata(L, cls);

			if (slot == LUAEXT_PROXY_OP_UNM) {
				lua_pushcclosure(L, luaext_proxy_unop, 1);
			} else {
				lua_pushinteger(L, slot);
				lua_pushcclosure(L, luaext_proxy_binop, 2);
			}

			lua_setfield(L, -2, luaext_proxy_ops[slot].metamethod);
		}
	}

	/*
	 * Intern map-first: the guard above keys on registry[cls], so that write
	 * must come LAST. Either interning step can raise on memory pressure, and
	 * a raise after registry[cls] existed would hand every later push a
	 * metatable the identity map never learned — permanently refusing the
	 * class's proxies. Torn the other way round, the next push simply builds
	 * afresh and the orphaned map entry is inert.
	 */
	luaext_proxy_mts_map(L);
	lua_pushvalue(L, -2);
	lua_pushlightuserdata(L, cls);
	lua_rawset(L, -3);
	lua_pop(L, 1);

	lua_pushvalue(L, -1);
	lua_rawsetp(L, LUA_REGISTRYINDEX, cls);
}

/*
 * The allocating half of pushing a proxy: a blank, magic-less shell wearing
 * `cls`'s metatable, with the GC list already grown to hold it. EVERY step
 * that can raise lives here, and the shell owns nothing — a raise leaves a
 * userdata whose zeroed magic makes the finaliser a no-op. The split exists
 * for .new, which must run all raising allocation BEFORE it creates the PHP
 * object whose sole reference would otherwise be stranded by the longjmp.
 */
static luaext_proxy_ud *luaext_proxy_push_shell(luaext_sandbox *sandbox, lua_State *L,
												luaext_proxy_class *cls)
{
	luaext_proxy_ud *slot;

	/* Reserved here, with the code that consumes it: a first push builds the
	 * metatable, whose __index table and per-method upvalues need more room
	 * than the conversion layer's per-level reservation was sized for. Both
	 * call sites run under a protected frame, so raising is the refusal. */
	luaL_checkstack(L, 8, "luaext: no stack to push a proxy");

	slot = (luaext_proxy_ud *)lua_newuserdatauv(L, sizeof(*slot), 0);

	memset(slot, 0, sizeof(*slot));

	luaext_proxy_push_metatable(L, cls);
	lua_setmetatable(L, -2);

	if (sandbox->proxy_gc_count == sandbox->proxy_gc_cap) {
		size_t cap = sandbox->proxy_gc_cap == 0 ? 8 : sandbox->proxy_gc_cap * 2;

		/* pemalloc never raises: on true OOM it ends the process rather than
		 * longjmping, so growing here keeps the binding half raise-free. */
		sandbox->proxy_gc_items = (luaext_proxy_ud **)perealloc(
			sandbox->proxy_gc_items, cap * sizeof(*sandbox->proxy_gc_items), 1);
		sandbox->proxy_gc_cap = cap;
	}

	return slot;
}

/* The binding half: no step in here can raise. */
static void luaext_proxy_bind(luaext_sandbox *sandbox, luaext_proxy_ud *slot,
							  luaext_proxy_class *cls, zend_object *object)
{
	slot->object = object;
	GC_ADDREF(object);
	slot->cls = cls;
	cls->live_proxies++;
	slot->gc_index = sandbox->proxy_gc_count;
	sandbox->proxy_gc_items[sandbox->proxy_gc_count++] = slot;

	/* Last: only a fully-listed, reference-holding payload is a live proxy. */
	slot->magic = LUAEXT_PROXY_MAGIC;
}

/*
 * Revoke a record's class-table anchor: clear the magic so every dispatch
 * closure that captured it refuses from this instant, and drop the registry
 * pin so the payload dies with the last closure. After this the record's
 * lifetime no longer depends on who still aliases the table — which is what
 * makes freeing it sound. Neither step allocates (luaL_unref only writes),
 * so it is safe wherever reclamation runs. A no-op for a table-less record.
 */
static void luaext_proxy_anchor_drop(lua_State *L, luaext_proxy_class *record)
{
	if (record->anchor != NULL) {
		record->anchor->magic = 0;
		record->anchor = NULL;
	}

	if (record->anchor_ref != LUA_NOREF) {
		luaL_unref(L, LUA_REGISTRYINDEX, record->anchor_ref);
		record->anchor_ref = LUA_NOREF;
	}
}

/*
 * Remove a record's metatable from the registry: the class -> metatable
 * interning and the metatable's entry in the proxymts identity map. Called
 * only when no live proxy wears the metatable, so the one thing that can
 * still hold it afterwards is a debugMutate-forged value — which the map
 * removal demotes to the refuse-everywhere shape a stripped proxy already
 * has. No step here allocates, so it is safe from a finaliser.
 */
static void luaext_proxy_scrub_metatable(lua_State *L, luaext_proxy_class *record)
{
	if (lua_rawgetp(L, LUA_REGISTRYINDEX, record) == LUA_TTABLE) {
		if (lua_rawgetp(L, LUA_REGISTRYINDEX, &luaext_key_proxymts) == LUA_TTABLE) {
			lua_pushvalue(L, -2);
			lua_pushnil(L);
			lua_rawset(L, -3);
		}

		lua_pop(L, 1);
	}

	lua_pop(L, 1);
	lua_pushnil(L);
	lua_rawsetp(L, LUA_REGISTRYINDEX, record);
}

/*
 * Free a retired record whose last proxy has died: unlink it from the
 * retired chain (retire-with-none-alive never linked it), scrub its
 * metatable, release its storage. Frees only C memory, so it is safe from a
 * finaliser too.
 */
static void luaext_proxy_discard(luaext_sandbox *sandbox, lua_State *L, luaext_proxy_class *record)
{
	luaext_proxy_class **link = &sandbox->proxy_retired;

	while (*link != NULL) {
		if (*link == record) {
			*link = record->next;
			break;
		}

		link = &(*link)->next;
	}

	luaext_proxy_scrub_metatable(L, record);
	luaext_proxy_class_free(record);
}

bool luaext_proxy_try_push(luaext_sandbox *sandbox, lua_State *L, zend_object *object)
{
	luaext_proxy_class *cls = luaext_proxy_find(sandbox, object->ce);

	if (cls == NULL) {
		return false;
	}

	luaext_proxy_bind(sandbox, luaext_proxy_push_shell(sandbox, L, cls), cls, object);

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
 * Operator mapping
 *
 * PHP has no operator protocol, so operators are mapped, never inferred: the
 * $operators parameter (which overrides everything), else per-method
 * #[LuaOperator] attributes. A mapping is granted for its operator only —
 * being mapped does not make a method callable by name.
 * ---------------------------------------------------------------------- */

/* Which slot an Operator enum case names. -1 is unreachable while the enum
 * and luaext_proxy_ops agree; guarded anyway. */
static int luaext_proxy_op_slot(zend_object *case_object)
{
	zend_string *case_name = Z_STR_P(zend_enum_fetch_case_name(case_object));
	int slot;

	for (slot = 0; slot < LUAEXT_PROXY_OP__COUNT; slot++) {
		if (zend_string_equals_cstr(case_name, luaext_proxy_ops[slot].case_name,
									strlen(luaext_proxy_ops[slot].case_name))) {
			return slot;
		}
	}

	return -1;
}

/* Validate one mapping and store it, or throw and return false. */
static bool luaext_proxy_map_operator(luaext_proxy_class *record, zend_class_entry *ce,
									  const zend_string *method_name, int slot)
{
	zend_function *method;

	if (slot < 0) {
		zend_throw_exception_ex(
			luaext_ce_configuration_error, 0,
			"The operator map for %s names an Operator this build does not know",
			ZSTR_VAL(ce->name));
		return false;
	}

	method = (zend_function *)zend_hash_str_find_ptr_lc(&ce->function_table, ZSTR_VAL(method_name),
														ZSTR_LEN(method_name));

	if (method == NULL) {
		zend_throw_exception_ex(luaext_ce_configuration_error, 0,
								"%s has no method %s() to map to an operator", ZSTR_VAL(ce->name),
								ZSTR_VAL(method_name));
		return false;
	}

	/* Before the method's own checks: a second mapping to a taken slot is a
	 * duplicate whatever the second method looks like. */
	if (record->op_methods[slot] != NULL) {
		zend_throw_exception_ex(luaext_ce_configuration_error, 0,
								"Operator %s is mapped to two methods of %s",
								luaext_proxy_ops[slot].case_name, ZSTR_VAL(ce->name));
		return false;
	}

	{
		/* Static is the one refusal method exposure does not share: statics
		 * back the class table happily, but an operator needs a receiver. */
		const char *refusal = (method->common.fn_flags & ZEND_ACC_STATIC)
								  ? "it is static"
								  : luaext_proxy_method_refusal(method);

		if (refusal != NULL) {
			zend_throw_exception_ex(luaext_ce_configuration_error, 0,
									"%s::%s() cannot back an operator: %s", ZSTR_VAL(ce->name),
									ZSTR_VAL(method_name), refusal);
			return false;
		}
	}

	if (luaext_proxy_ops[slot].unary) {
		if (method->common.required_num_args != 0) {
			zend_throw_exception_ex(luaext_ce_configuration_error, 0,
									"%s::%s() must take no required parameters to back unary minus",
									ZSTR_VAL(ce->name), ZSTR_VAL(method_name));
			return false;
		}
	} else if (method->common.required_num_args != 1) {
		zend_throw_exception_ex(
			luaext_ce_configuration_error, 0,
			"%s::%s() must take exactly one required parameter to back a binary operator",
			ZSTR_VAL(ce->name), ZSTR_VAL(method_name));
		return false;
	}

	/*
	 * Comparisons must DECLARE bool: the metamethod's answer becomes control
	 * flow, and a mapping that could return anything else is a mistake best
	 * refused at registration. A vendor method without the declaration is
	 * mapped through a wrapper-subclass override that adds it.
	 */
	if (luaext_proxy_ops[slot].comparison) {
		bool declared = (method->common.fn_flags & ZEND_ACC_HAS_RETURN_TYPE) != 0;

		if (declared) {
			zend_type return_type = method->common.arg_info[-1].type;

			declared = !ZEND_TYPE_IS_COMPLEX(return_type) &&
					   ZEND_TYPE_PURE_MASK(return_type) == MAY_BE_BOOL;
		}

		if (!declared) {
			zend_throw_exception_ex(
				luaext_ce_configuration_error, 0,
				"%s::%s() must declare a bool return to back a comparison operator",
				ZSTR_VAL(ce->name), ZSTR_VAL(method_name));
			return false;
		}
	}

	record->op_methods[slot] = method;

	return true;
}

/*
 * The full mapping pass. Two sources MERGE, and that is deliberate: the map
 * (the $operators parameter, else the #[LuaClass] field) exists to reach
 * inherited vendor methods that cannot carry attributes, while a class's own
 * methods speak for themselves with #[LuaOperator] — a wrapper subclass
 * routinely needs both at once. A slot claimed by both sources is refused as
 * the duplicate it is.
 */
static bool luaext_proxy_collect_operators(luaext_proxy_class *record, zend_class_entry *ce,
										   HashTable *operators)
{
	zend_string *marker;
	zend_function *method;
	bool mapped = true;

	if (operators != NULL) {
		zend_string *method_name;
		zval *op_value;

		ZEND_HASH_FOREACH_STR_KEY_VAL(operators, method_name, op_value)
		{
			ZVAL_DEREF(op_value);

			if (method_name == NULL || ZSTR_LEN(method_name) == 0 ||
				Z_TYPE_P(op_value) != IS_OBJECT ||
				!instanceof_function(Z_OBJCE_P(op_value), luaext_ce_operator)) {
				zend_throw_exception_ex(
					luaext_ce_configuration_error, 0,
					"The operator map for %s must map method names to Operator cases",
					ZSTR_VAL(ce->name));
				return false;
			}

			if (!luaext_proxy_map_operator(record, ce, method_name,
										   luaext_proxy_op_slot(Z_OBJ_P(op_value)))) {
				return false;
			}
		}
		ZEND_HASH_FOREACH_END();
	}

	marker = zend_string_tolower(luaext_ce_lua_operator_attribute->name);

	ZEND_HASH_MAP_FOREACH_PTR(&ce->function_table, method)
	{
		zend_attribute *attribute = zend_get_attribute(method->common.attributes, marker);
		zend_string *filename = NULL;
		zval marker_object;
		zval holder;
		zval *configured;
		int slot = -1;

		if (attribute == NULL) {
			continue;
		}

		if (ce->type == ZEND_USER_CLASS) {
			filename = ce->info.user.filename;
		}

		if (zend_get_attribute_object(&marker_object, luaext_ce_lua_operator_attribute, attribute,
									  ce, filename) != SUCCESS) {
			mapped = false;
			break;
		}

		configured = zend_read_property(luaext_ce_lua_operator_attribute, Z_OBJ(marker_object),
										ZEND_STRL("operator"), true, &holder);

		if (configured != NULL && Z_TYPE_P(configured) == IS_OBJECT) {
			slot = luaext_proxy_op_slot(Z_OBJ_P(configured));
		}

		zval_ptr_dtor(&marker_object);

		mapped = luaext_proxy_map_operator(record, ce, method->common.function_name, slot);

		if (!mapped) {
			break;
		}
	}
	ZEND_HASH_FOREACH_END();

	zend_string_release(marker);

	return mapped;
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

/* The pipeline past class resolution: everything that needs a vetted ce. */
static bool luaext_proxy_register_with(luaext_sandbox *sandbox, zend_class_entry *ce,
									   HashTable *allowlist, zend_string *lua_name,
									   HashTable *operators)
{
	luaext_proxy_class *walk;
	luaext_proxy_class *record;
	zend_string *resolved_name;
	bool collected;

	/*
	 * Re-checked here, not only at the method boundary: resolving the class
	 * ran the autoloader and evaluating #[LuaClass] instantiated its carrier —
	 * both arbitrary PHP that may have closed this very sandbox.
	 */
	if (!luaext_sandbox_check_usable(sandbox)) {
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

	/*
	 * The claim table covers every registrar — registerLibrary(),
	 * registerObject(), and this — so no registration can ever silently
	 * overwrite another's global. Checked here, claimed only at the very end:
	 * a refused registration must burn nothing.
	 */
	if (!luaext_sandbox_global_available(sandbox, ZSTR_VAL(resolved_name),
										 ZSTR_LEN(resolved_name))) {
		zend_string_release(resolved_name);
		return false;
	}

	record = pecalloc(1, sizeof(*record), 1);
	record->ce = ce;
	record->lua_name = resolved_name;
	record->anchor_ref = LUA_NOREF;
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

	if (collected) {
		collected = luaext_proxy_collect_operators(record, ce, operators);
	}

	if (collected && zend_hash_num_elements(record->instance_methods) == 0 &&
		zend_hash_num_elements(record->static_methods) == 0 && record->constructor == NULL &&
		record->to_string == NULL) {
		bool has_operator = false;
		int slot;

		for (slot = 0; slot < LUAEXT_PROXY_OP__COUNT; slot++) {
			if (record->op_methods[slot] != NULL) {
				has_operator = true;
				break;
			}
		}

		if (!has_operator) {
			zend_throw_exception_ex(luaext_ce_configuration_error, 0,
									"Nothing of %s is exposed: no method carries #[LuaMethod], no "
									"allowlist was given, and no operator is mapped",
									ZSTR_VAL(ce->name));
			collected = false;
		}
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

		/* The running state can be a coroutine with no ambient slack; refuse
		 * like the sibling registrars rather than trust it. */
		if (!lua_checkstack(L, 4)) {
			zend_throw_exception(luaext_ce_memory_limit_error,
								 "Cannot register a class: the interpreter stack cannot grow", 0);
			luaext_proxy_class_free(record);
			return false;
		}

		lua_pushcfunction(L, luaext_proxy_plant_table);
		lua_pushlightuserdata(L, record);
		status = lua_pcall(L, 1, 0, 0);

		if (status != LUA_OK) {
			luaext_error_throw_from_lua(sandbox, L, status);
			/* The half-built table is garbage, but the anchor was committed
			 * and pinned the moment it existed; revoke it with the record. */
			luaext_proxy_anchor_drop(L, record);
			luaext_proxy_class_free(record);
			return false;
		}
	}

	record->next = sandbox->proxy_classes;
	sandbox->proxy_classes = record;

	/* The name is reserved whether or not a table was planted: an
	 * instance-only class still owns its identity. */
	luaext_sandbox_global_claim(sandbox, ZSTR_VAL(record->lua_name), ZSTR_LEN(record->lua_name));

	return true;
}

bool luaext_proxy_register(luaext_sandbox *sandbox, zend_string *class_name, HashTable *allowlist,
						   zend_string *lua_name, HashTable *operators)
{
	zend_class_entry *ce;
	zval carrier;
	bool registered;

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

	/*
	 * The #[LuaClass] carrier: each explicit argument that was NOT given falls
	 * back to the attribute's field. A carrier, never a grant — an annotated
	 * class still crosses nothing until a sandbox reaches this function. The
	 * attribute object owns the fallback values, so it stays alive across the
	 * whole pipeline below.
	 */
	ZVAL_UNDEF(&carrier);

	{
		zend_string *marker = zend_string_tolower(luaext_ce_lua_class_attribute->name);
		zend_attribute *attribute = zend_get_attribute(ce->attributes, marker);

		zend_string_release(marker);

		if (attribute != NULL) {
			zend_string *filename = ce->type == ZEND_USER_CLASS ? ce->info.user.filename : NULL;
			zval holder;
			zval *field;

			if (zend_get_attribute_object(&carrier, luaext_ce_lua_class_attribute, attribute, ce,
										  filename) != SUCCESS) {
				return false;
			}

			if (lua_name == NULL) {
				field = zend_read_property(luaext_ce_lua_class_attribute, Z_OBJ(carrier),
										   ZEND_STRL("luaName"), true, &holder);

				if (field != NULL && Z_TYPE_P(field) == IS_STRING) {
					lua_name = Z_STR_P(field);
				}
			}

			if (allowlist == NULL) {
				field = zend_read_property(luaext_ce_lua_class_attribute, Z_OBJ(carrier),
										   ZEND_STRL("methods"), true, &holder);

				if (field != NULL && Z_TYPE_P(field) == IS_ARRAY) {
					allowlist = Z_ARRVAL_P(field);
				}
			}

			if (operators == NULL) {
				field = zend_read_property(luaext_ce_lua_class_attribute, Z_OBJ(carrier),
										   ZEND_STRL("operators"), true, &holder);

				if (field != NULL && Z_TYPE_P(field) == IS_ARRAY) {
					operators = Z_ARRVAL_P(field);
				}
			}
		}
	}

	registered = luaext_proxy_register_with(sandbox, ce, allowlist, lua_name, operators);

	if (!Z_ISUNDEF(carrier)) {
		zval_ptr_dtor(&carrier);
	}

	return registered;
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

void luaext_proxy_retire_name(luaext_sandbox *sandbox, lua_State *L, const zend_string *lua_name)
{
	luaext_proxy_class **link = &sandbox->proxy_classes;

	while (*link != NULL) {
		luaext_proxy_class *record = *link;

		if (zend_string_equals(record->lua_name, lua_name)) {
			*link = record->next;
			record->retired = true;

			/* First, so a stale class-table alias refuses from this instant
			 * and the record's fate stops depending on who still holds one. */
			luaext_proxy_anchor_drop(L, record);

			if (record->live_proxies == 0) {
				/* Nothing dispatches through it: gone now, metatable and
				 * all, so swap loops cannot accumulate dead records. */
				record->next = NULL;
				luaext_proxy_scrub_metatable(L, record);
				luaext_proxy_class_free(record);
				return;
			}

			/* Retired, not freed: the metatable's closures still reference
			 * this record through light userdata, and proxies a script
			 * already holds keep dispatching through them. The last such
			 * proxy's finaliser reclaims it. */
			record->next = sandbox->proxy_retired;
			sandbox->proxy_retired = record;
			return;
		}

		link = &record->next;
	}
}

void luaext_proxy_release_abandoned(luaext_sandbox *sandbox)
{
	while (sandbox->proxy_gc_count > 0) {
		luaext_proxy_ud *slot = sandbox->proxy_gc_items[--sandbox->proxy_gc_count];
		zval carrier;

		if (slot->magic != LUAEXT_PROXY_MAGIC || slot->object == NULL) {
			continue;
		}

		slot->magic = 0;
		ZVAL_OBJ(&carrier, slot->object);
		slot->object = NULL;

		/* Direct, not deferred: the defer queue exists to move releases out
		 * of a running collector, and no collector can run in a state that
		 * will never execute again. */
		zval_ptr_dtor(&carrier);
	}
}

static void luaext_proxy_free_chain(luaext_proxy_class *record)
{
	while (record != NULL) {
		luaext_proxy_class *next = record->next;

		luaext_proxy_class_free(record);
		record = next;
	}
}

void luaext_proxy_shutdown(luaext_sandbox *sandbox)
{
	luaext_proxy_free_chain(sandbox->proxy_classes);
	sandbox->proxy_classes = NULL;

	luaext_proxy_free_chain(sandbox->proxy_retired);
	sandbox->proxy_retired = NULL;

	/* lua_close() finalised every proxy, so the list is empty by now; the
	 * storage is what remains. */
	if (sandbox->proxy_gc_items != NULL) {
		pefree(sandbox->proxy_gc_items, 1);
		sandbox->proxy_gc_items = NULL;
	}

	sandbox->proxy_gc_count = 0;
	sandbox->proxy_gc_cap = 0;
}
