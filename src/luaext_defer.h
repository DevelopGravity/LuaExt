/*
 * luaext — releasing PHP references outside Lua's collector.
 *
 * THE PROBLEM, stated once: a Lua __gc finaliser runs inside the collector, and
 * several of ours drop the last reference to a PHP object. Dropping it calls
 * that object's __destruct, which is arbitrary user code, and arbitrary user
 * code may call back into the very sandbox whose collector is mid-sweep.
 * Re-entering a lua_State from inside its own GC is undefined behaviour: the
 * collector is walking structures the re-entrant call is free to mutate.
 *
 * It is not hypothetical. Any host that writes
 *
 *     $sandbox->registerLibrary('svc', ['go' => [$service, 'method']]);
 *
 * hands the closure storage a reference to $service. When Lua collects the
 * closure, zend_fcc_dtor() may run ~Service::__destruct(), and nothing stops
 * that destructor touching the sandbox.
 *
 * THE FIX: finalisers hand ownership to this queue instead of releasing, and the
 * queue drains at a point where no Lua execution is in progress. What the
 * finaliser gives up is only the *timing* of the release, which nothing depends
 * on -- Lua already made no promise about when __gc runs.
 *
 * Drain points, both in luaext_sandbox.c:
 *   - the outermost return from Lua, where in_lua falls to zero
 *   - immediately after lua_close(), where the state no longer exists at all
 *
 * The queue is persistent (pemalloc) rather than request-allocated, for the same
 * reason the closure's name is: __gc also runs from lua_close() during the
 * request-shutdown sweep, by which point the request allocator is gone.
 */

#ifndef LUAEXT_DEFER_H
#define LUAEXT_DEFER_H

#include "luaext_types.h"

/*
 * Take ownership of `fcc` and release it at the next drain.
 *
 * Returns false only if the queue could not grow, in which case the caller must
 * release immediately and accept the re-entrancy risk -- refusing to release at
 * all would leak, and a leak is worse than a narrow window.
 */
bool luaext_defer_fcc(luaext_sandbox *sandbox, zend_fcall_info_cache *fcc);

/*
 * Take ownership of `value` and release it at the next drain. `value` is left
 * UNDEF, so the caller cannot double-release it.
 *
 * Same false semantics as above.
 */
bool luaext_defer_zval(luaext_sandbox *sandbox, zval *value);

/*
 * Release everything queued so far.
 *
 * Re-entrant by construction: the pending items are detached before any of them
 * is released, so a destructor that calls back into this sandbox and triggers
 * another drain finds an empty queue rather than the list it is being released
 * from.
 */
void luaext_defer_drain(luaext_sandbox *sandbox);

/* Release the queue's own storage. Drains first. */
void luaext_defer_shutdown(luaext_sandbox *sandbox);

#endif /* LUAEXT_DEFER_H */
