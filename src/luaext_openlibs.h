/*
 * luaext — assembling the standard library a sandbox may see.
 *
 * The rule this file exists to enforce: **a library table Lua can reach is
 * built by us**. Upstream's opener runs into a scratch table that is never
 * assigned to a global and never written into _LOADED, and we copy the approved
 * members out of it. The scratch table is garbage before any script runs.
 *
 * That is not the same as deleting members afterwards, and the difference is
 * not pedantic -- it is the direction of failure when Lua is upgraded:
 *
 *   deny-list: 5.5.2 adds string.frobnicate, and it silently appears in every
 *              untrusted sandbox.
 *   allow-list: it is not selected, and the drift check fails the build until
 *              somebody classifies it.
 *
 * SECURITY.md already cites table.move as exactly this lesson.
 *
 * -------------------------------------------------------------------------
 * INVARIANT, for every lua_pcall in src/, not only the ones here:
 *
 *   On failure it must either convert the error into a PHP exception that
 *   reaches the host, or re-raise it. It may NEVER return normally having
 *   discarded a fatal.
 *
 * A fatal error is how a limit is enforced. A protected call that swallows one
 * is a limit that does not exist.
 * ---------------------------------------------------------------------- */

#ifndef LUAEXT_OPENLIBS_H
#define LUAEXT_OPENLIBS_H

#include "luaext_types.h"

#include <lua.h>

/* -------------------------------------------------------------------------
 * Describing a library
 * ---------------------------------------------------------------------- */

/*
 * One member a sandbox may see.
 *
 * `require_caps` is a luaext_cap bitset; EVERY bit in it must be granted for
 * the member to be selected. Zero means unconditional.
 */
typedef struct {
	const char *name;
	uint32_t require_caps;
} luaext_member;

/*
 * Install replacements, and anything else that needs the originals.
 *
 * Called with the selected table on the top of the stack and the scratch table
 * directly below it, so a decorator may lift an upstream closure out of scratch
 * and keep it as an upvalue -- which is how the wrappers over `collectgarbage`,
 * `load` and `math.randomseed` reach the implementation they delegate to
 * without re-deriving it.
 *
 * Runs inside the installer's protected call, so it may raise. Must leave the
 * stack as it found it.
 */
typedef bool (*luaext_decorator)(lua_State *L, luaext_sandbox *sandbox);

typedef struct {
	/* The global the selected table is published as; NULL means the globals
	 * table itself (the base library). */
	const char *global;

	/* Upstream's opener, or NULL for a library we build ourselves. */
	lua_CFunction opener;

	/* Whole-library gate; every bit must be granted. Zero means unconditional. */
	uint32_t require_caps;

	/* {NULL, 0}-terminated. A name listed here that is absent from scratch is a
	 * hard failure: it catches an upgrade that renames or removes a member. */
	const luaext_member *allow;

	/* NULL-terminated. Known to exist upstream and deliberately never exposed
	 * at any capability level. Everything in scratch must be in `allow` or
	 * here, or construction fails -- see luaext_openlibs_check_drift(). */
	const char *const *withheld;

	luaext_decorator decorate;
} luaext_library;

/* -------------------------------------------------------------------------
 * The installer
 * ---------------------------------------------------------------------- */

/*
 * Build and publish every library this sandbox's capabilities allow.
 *
 * Called once from construction, before any script exists. Throws a PHP
 * exception and returns false on failure; the caller must abandon the sandbox.
 *
 * The whole install runs under one lua_pcall: it allocates far more than the
 * Wave-1 placeholder did, and an unprotected memory error here would reach
 * lua_atpanic and take the request down.
 */
bool luaext_openlibs_install(luaext_sandbox *sandbox);

/* -------------------------------------------------------------------------
 * Helpers, for the per-library builders
 * ---------------------------------------------------------------------- */

/*
 * Run `opener` into a fresh table and push it. Deliberately not
 * luaL_requiref(): that writes _LOADED[name] and _G[name], publishing the
 * unfiltered table -- and _LOADED becomes package.loaded once require() lands.
 */
void luaext_openlibs_scratch(lua_State *L, lua_CFunction opener, const char *name);

/*
 * Push a new table holding every member of `allow` whose capabilities are
 * granted, read raw from the scratch table at `scratch_index`.
 *
 * Returns the number selected, so a caller can publish nothing rather than an
 * empty table -- `debug` with no debug capability must be nil, not {}.
 *
 * Raises if a named member is absent from scratch.
 */
/*
 * A withheld member's stand-in: a truthy function whose only behaviour is to
 * raise FeatureNotGrantedError naming `feature` and `capability`. Shared by
 * the select pass and the hand-built os/io/debug installers.
 */
void luaext_openlibs_push_gate_stub(lua_State *L, const char *feature, const char *capability);

/* The Capabilities property name for the first bit set in `missing`. */
const char *luaext_openlibs_capability_name(uint32_t missing);

int luaext_openlibs_select(lua_State *L, luaext_sandbox *sandbox, int scratch_index,
						   const luaext_member *allow, const char *library_global);

/*
 * Verify every string key in the scratch table at `scratch_index` is accounted
 * for by `allow` or `withheld`. An unknown key raises, naming the member and
 * the file to edit.
 *
 * Aggressive on purpose. A vendored-Lua bump is a deliberate act,
 * tools/check-lua-upstream.sh already announces one, and the alternatives are
 * silent exposure or silent withholding.
 */
void luaext_openlibs_check_drift(lua_State *L, int scratch_index, const luaext_member *allow,
								 const char *const *withheld, const char *library);

/* -------------------------------------------------------------------------
 * Libraries built elsewhere
 *
 * These are the only things the installer knows about the debug and os
 * libraries. Both build their table from scratch and publish it themselves, or
 * publish nothing when no capability calls for it.
 *
 * Called from inside the installer's protected call; may raise. Must leave the
 * stack as found.
 * ---------------------------------------------------------------------- */

bool luaext_debuglib_install(lua_State *L, luaext_sandbox *sandbox);
bool luaext_oslib_install(lua_State *L, luaext_sandbox *sandbox);

#endif /* LUAEXT_OPENLIBS_H */
