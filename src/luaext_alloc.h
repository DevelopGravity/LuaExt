/*
 * luaext — memory accounting.
 *
 * Every byte a script can cause to be allocated passes through here, whether
 * Lua allocated it or the host did on the script's behalf. That is what makes
 * Limits::$memoryBytes a real ceiling rather than a number in a struct.
 */

#ifndef LUAEXT_ALLOC_H
#define LUAEXT_ALLOC_H

#include "luaext_types.h"

/*
 * The lua_Alloc handed to lua_newstate. `ud` is the owning luaext_sandbox.
 *
 * Refuses any request that would push usage + charged past the limit, which
 * Lua reports to the script as a memory error. Backed by malloc, not the Zend
 * allocator: a sandbox may outlive the request that built it in a worker SAPI.
 *
 * Named for what it is rather than luaext_alloc, which is the accounting
 * struct in luaext_types.h.
 */
void *luaext_lua_alloc(void *ud, void *ptr, size_t osize, size_t nsize);

/*
 * Bill bytes the sandbox holds outside the Lua heap — VFS file buffers, the
 * output buffer — against the same ceiling.
 *
 * These are invisible to lua_Alloc, so without this a script could exhaust
 * host memory through the filesystem or by printing while staying comfortably
 * inside its Lua budget.
 *
 * Returns false when the charge would exceed the limit, in which case nothing
 * is charged and the caller must fail the operation rather than proceed.
 */
bool luaext_alloc_charge(luaext_sandbox *sandbox, size_t bytes);

/* Release bytes previously charged. Never called with more than is charged. */
void luaext_alloc_discharge(luaext_sandbox *sandbox, size_t bytes);

/* Live bytes: Lua heap plus host-side charges. */
size_t luaext_alloc_usage(const luaext_sandbox *sandbox);

/* Highest value luaext_alloc_usage() has reached. */
size_t luaext_alloc_peak(const luaext_sandbox *sandbox);

/*
 * Change the ceiling at runtime (Sandbox::setLimits). Zero lifts it.
 *
 * Lowering below current usage is allowed and does not retroactively fail:
 * the next allocation that would grow the heap is refused instead, which is
 * the only point at which refusing is safe.
 */
void luaext_alloc_set_limit(luaext_sandbox *sandbox, size_t limit);

/*
 * Re-tune the collector for how close the sandbox is to its ceiling.
 *
 * Lua 5.5 replaced the old LUA_GCSETPAUSE/LUA_GCSETSTEPMUL calls with
 * LUA_GCPARAM, so this is written against the new API rather than ported.
 * Called from the allocator as usage moves; collecting harder as a script
 * approaches its limit is what stops a recoverable peak from becoming a
 * memory error.
 */
void luaext_alloc_tune_gc(luaext_sandbox *sandbox);

#endif /* LUAEXT_ALLOC_H */
