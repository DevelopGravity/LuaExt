/*
 * luaext — require(), and the `package` table it reads.
 *
 * SIX STEPS, IN THIS ORDER, and the order is the specification rather than an
 * implementation detail (docs/lua-api.md states it too):
 *
 *   1. package.loaded      already resolved, keyed by name
 *   2. circular guard      a module requiring itself fails cleanly
 *   3. the two limits      maxModules and maxRequireDepth
 *   4. package.preload     loaders the host registered
 *   5. the VFS             searched along SandboxConfig::$modulePaths
 *   6. ModuleResolver      a PHP fallback, asked last
 *
 * WHAT `package` IS NOT. Upstream's package table is a loader toolkit: cpath,
 * searchers, loadlib. Every one of those exists to reach a shared object, and a
 * sandbox that can dlopen has no boundary left. Ours exposes exactly three
 * names -- loaded, preload, and a read-only path -- and there is no searchers
 * list a script can append to, because a script that could add a searcher could
 * make require() call anything it liked.
 *
 * A MODULE THAT FAILS IS NOT CACHED. Upstream stores a sentinel while a module
 * loads and leaves whatever the loader produced behind on error; here a failed
 * load is removed, so a later require() gets a fresh attempt rather than
 * replaying a cached failure. The circular guard is a separate table for that
 * reason -- using package.loaded as both the cache and the in-progress marker
 * is what makes those two behaviours impossible to separate.
 *
 * NAMES ARE VALIDATED BEFORE ANYTHING SEES THEM. A module name reaches the VFS
 * as part of a path, so it answers to the same rule the path canonicaliser
 * does: [A-Za-z0-9_.-] only, 128 bytes, and no ".." segment. Rejecting here
 * means neither the search paths nor a host resolver can be handed a name that
 * escapes what the host meant to expose.
 */

#ifndef LUAEXT_REQUIRE_H
#define LUAEXT_REQUIRE_H

#include "luaext_types.h"

/* The longest module name accepted, before any path substitution. */
#define LUAEXT_REQUIRE_MAX_NAME 128u

/*
 * Install `require` and `package`, when the require capability is granted.
 *
 * Absent rather than present-and-failing without it, for the reason io.open is:
 * a script can test for it, and a stub that always refused would be
 * indistinguishable from a resolver that happens to find nothing.
 */
bool luaext_require_install(lua_State *L, luaext_sandbox *sandbox);

/*
 * Put a host loader into package.preload, backing Sandbox::preloadModule().
 *
 * `loader` is already on the Lua stack top and is consumed. Returns false with a
 * PHP exception thrown when the name is not one require() would accept -- a host
 * that preloads a name no script can ask for has made a mistake worth reporting
 * at the point of the mistake.
 */
bool luaext_require_preload(lua_State *L, luaext_sandbox *sandbox, const char *name,
							size_t name_len);

/* Release what the subsystem owns. Safe on a sandbox that never installed it. */
void luaext_require_shutdown(luaext_sandbox *sandbox);

/*
 * Attach the configured resolver and search paths.
 *
 * Takes its own references rather than reaching back through config_zv, the way
 * the VFS and the output sink do, so teardown ordering cannot strand them.
 */
bool luaext_require_init_from_config(luaext_sandbox *sandbox, zval *config);

#endif /* LUAEXT_REQUIRE_H */
