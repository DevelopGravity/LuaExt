/*
 * luaext — PHP object proxies.
 *
 * A host registers a class per sandbox with Sandbox::registerClass(); from
 * then on instances of that class (or its subclasses) cross into Lua as full
 * userdata proxies instead of being refused by the conversion layer. This
 * header owns the registry of registered classes; the proxy userdata itself,
 * its metatables and its dispatch closures build on these records.
 *
 * Trust model, restated from the spec so nobody widens it by accident:
 * registration is the grant, is per sandbox, and is one-way for the sandbox's
 * lifetime. Attributes on a class are configuration carriers, never grants.
 */

#ifndef LUAEXT_PROXY_H
#define LUAEXT_PROXY_H

#include "luaext_types.h"

/*
 * Lua operator slots a mapped method may back. Mirrors the Operator enum in
 * the stub; luaext_proxy.c owns the name table that ties the two together.
 */
typedef enum {
	LUAEXT_PROXY_OP_LT = 0,
	LUAEXT_PROXY_OP_LE,
	LUAEXT_PROXY_OP_EQ,
	LUAEXT_PROXY_OP_ADD,
	LUAEXT_PROXY_OP_SUB,
	LUAEXT_PROXY_OP_MUL,
	LUAEXT_PROXY_OP_DIV,
	LUAEXT_PROXY_OP_MOD,
	LUAEXT_PROXY_OP_POW,
	LUAEXT_PROXY_OP_UNM,
	LUAEXT_PROXY_OP_CONCAT,
	LUAEXT_PROXY_OP__COUNT
} luaext_proxy_op;

/*
 * One registered class. Everything in here is persistent (pemalloc'd, or
 * persistent zend_strings, or engine-owned pointers): the registry is torn
 * down by luaext_proxy_shutdown() on the close path, which can run from the
 * RSHUTDOWN sweep.
 *
 * The zend_function pointers are borrowed from the class's own function
 * table, which outlives every sandbox on the thread; the zend_class_entry is
 * borrowed the same way.
 */
struct luaext_proxy_class {
	zend_class_entry *ce;

	/* The global-table name. Always recorded — collision checks apply even to
	 * a class that plants no table — but a table is only created when statics
	 * or a constructor exist. */
	zend_string *lua_name;

	/* lua name (persistent zend_string) -> zend_function*, no value dtor. */
	HashTable *instance_methods;
	HashTable *static_methods;

	/* NULL unless exposed. */
	zend_function *constructor;
	zend_string *constructor_lua_name; /* "new" or the #[LuaMethod] override */

	/* NULL unless __toString is marked or allowlisted. */
	zend_function *to_string;

	/* NULL = slot unmapped. Filled by operator validation. */
	zend_function *op_methods[LUAEXT_PROXY_OP__COUNT];

	struct luaext_proxy_class *next;
};

/*
 * The proxy payload: a full userdata wrapping one PHP object.
 *
 * `object` holds one refcount from the moment the proxy is pushed; __gc hands
 * it to the defer queue rather than releasing in the collector. `gc_index` is
 * the payload's slot in the sandbox's live-proxy list, which is what get_gc
 * reports to PHP's cycle collector and stats() counts.
 */
#define LUAEXT_PROXY_MAGIC 0x4C585072u /* "LXPr" */

struct luaext_proxy_ud {
	uint32_t magic;
	zend_object *object;	 /* NULL once finalised */
	luaext_proxy_class *cls; /* the nearest registered ancestor it wrapped as */
	size_t gc_index;
};

/*
 * Push a proxy for `object` if its class (or an ancestor) is registered.
 * Returns false — pushing nothing — when unregistered, so the caller falls
 * through to the conversion layer's refusal. May raise on memory pressure;
 * callers are already inside a raise-safe conversion context.
 */
bool luaext_proxy_try_push(luaext_sandbox *sandbox, lua_State *L, zend_object *object);

/*
 * The live proxy at `index`, or NULL for anything else. Never raises and
 * never dereferences foreign memory. Metamethods hand this ANY value — under
 * debugMutate a script can stamp a real proxy metatable onto a plain table —
 * so the gates run in this order, each one making the next read sound:
 *   1. lua_type(L, index) == LUA_TUSERDATA
 *   2. lua_rawlen(L, index) == sizeof(luaext_proxy_ud)
 *   3. the value's metatable answers in the luaext_key_proxymts map
 *   4. magic == LUAEXT_PROXY_MAGIC and object != NULL (resurrected is dead)
 */
luaext_proxy_ud *luaext_proxy_test(luaext_sandbox *sandbox, lua_State *L, int index);

/*
 * Report every live proxy's wrapped object to the cycle collector. Without
 * this, a script-held proxy of an object that (transitively) references its
 * own Sandbox is a cycle no collector can see, and the whole sandbox leaks
 * until process end.
 */
void luaext_proxy_add_gc(const luaext_sandbox *sandbox, zend_get_gc_buffer *buffer);

/*
 * Resolve, validate and store a registration.
 *
 * `class_name` is looked up (autoloading applies); `allowlist` overrides
 * method attributes exactly as registerObject()'s does; `lua_name` overrides
 * the default unqualified class name; `operators` maps method names to
 * Operator enum cases. NULL means "not given" for each.
 *
 * Returns false with a ConfigurationError (or the autoloader's own
 * exception) thrown; on refusal nothing is stored and the script-visible
 * world is untouched.
 */
bool luaext_proxy_register(luaext_sandbox *sandbox, zend_string *class_name, HashTable *allowlist,
						   zend_string *lua_name, HashTable *operators);

/*
 * The record a crossing instance wraps as: the class itself, or its nearest
 * registered ancestor walking the parent chain. NULL when unregistered —
 * the caller falls through to the conversion layer's refusal.
 */
luaext_proxy_class *luaext_proxy_find(const luaext_sandbox *sandbox, const zend_class_entry *ce);

/*
 * Retire the class registered under `lua_name`, if any: unlink it from the
 * find chain so NEW instances stop wrapping, while the record itself stays
 * allocated — its metatable and dispatch closures still reference it, and
 * proxies a script already holds keep working, because an object a script
 * was given cannot be taken back. A no-op for non-class names.
 */
void luaext_proxy_retire_name(luaext_sandbox *sandbox, const zend_string *lua_name);

/*
 * Release every PHP reference the live proxies still hold, for the one close
 * path whose finalisers never run: a bailout-abandoned state (in_lua above
 * zero), where lua_close() is refused and the heap is deliberately lost.
 * The leaked heap is exactly what keeps the listed payloads valid to read.
 * Destructors run here as ordinary host code; the sandbox is already marked
 * closed, so one that re-enters meets a ClosedSandboxError.
 */
void luaext_proxy_release_abandoned(luaext_sandbox *sandbox);

/* Free the registry — live and retired — and everything the records own. */
void luaext_proxy_shutdown(luaext_sandbox *sandbox);

#endif /* LUAEXT_PROXY_H */
