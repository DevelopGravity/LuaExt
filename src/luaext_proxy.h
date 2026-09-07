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

/* Free the registry and everything the records own. Idempotent. */
void luaext_proxy_shutdown(luaext_sandbox *sandbox);

#endif /* LUAEXT_PROXY_H */
