/*
 * luaext — the PHP side of the boundary.
 *
 * Lua calls out to PHP through a single C closure. The contract the docs
 * promise: a callback throwing RuntimeError becomes a Lua-catchable error, and
 * anything else aborts the whole execution with the original exception object
 * preserved for the host.
 */

#ifndef LUAEXT_PHPCALL_H
#define LUAEXT_PHPCALL_H

#include "luaext_types.h"

/*
 * Push a Lua C closure that invokes `callable`. The fcall_info_cache is copied
 * into the closure's own storage and released when Lua collects it, so the
 * caller keeps ownership of nothing.
 */
bool luaext_phpcall_push(luaext_sandbox *sandbox, zval *callable, const char *name);

/*
 * Build a table of callables and assign it to a global. Used by both
 * registerLibrary (explicit map) and registerObject (bound methods).
 */
bool luaext_phpcall_register_table(luaext_sandbox *sandbox, const char *name, size_t name_len,
								   HashTable *functions);

/*
 * Collect the methods of `instance` that are exposed to Lua: either the
 * explicit allowlist, or every method carrying #[LuaMethod]. Neither present is
 * a ConfigurationError -- exposing an object's whole surface by default is how
 * a host accidentally hands a script its own internals.
 *
 * Returns a hash of lua name => bound callable zval, or NULL with an exception
 * thrown. The caller owns the table and releases it with zend_array_destroy();
 * luaext_phpcall_register_table() only reads it.
 */
HashTable *luaext_phpcall_collect_methods(zval *instance, HashTable *allowlist);

#endif /* LUAEXT_PHPCALL_H */
