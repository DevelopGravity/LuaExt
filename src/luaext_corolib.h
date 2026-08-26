/*
 * luaext — the coroutine library, wrapped.
 *
 * Upstream's luaopen_coroutine is never installed, and LUAEXT_LIB_CORO stays
 * clear even for a trusted sandbox. Two things it does not do, both of which
 * matter here:
 *
 *   coroutine.resume is a protected call wearing a different hat. It returns
 *   `false, err` rather than propagating, which is the same shape as every other
 *   construct that can swallow a limit breach -- so upstream's resume lets a
 *   script move an infinite loop into a coroutine and catch its own CPU limit.
 *   Ours re-raises a fatal, keyed on the STATUS as well as the error value for
 *   exactly the reason pcall is (see luaext_baselib.c): LUA_ERRMEM carries Lua's
 *   own preallocated string, not our marker.
 *
 *   Nothing bounds how many coroutines exist or how deeply resume nests.
 *   Limits::$maxLiveCoroutines and $maxCoroutineDepth are enforced here.
 *
 * The third guarantee is a lifecycle one and lives at the call boundary rather
 * than in this file's functions: no suspended Lua execution state survives the
 * PHP call that created it. See luaext_corolib_sweep().
 */

#ifndef LUAEXT_COROLIB_H
#define LUAEXT_COROLIB_H

#include "luaext_types.h"

/*
 * Build the `coroutine` table and set it as a global.
 *
 * Installed unconditionally when the coroutines capability is granted, which is
 * the untrusted default -- coroutines are a language feature, not a privilege.
 * Returns false with a Lua error pending if the table cannot be assembled.
 */
bool luaext_corolib_install(lua_State *L, luaext_sandbox *sandbox);

/*
 * Force-close every coroutine this sandbox still has alive.
 *
 * MUST be called inside the timing bracket -- after the protected call returns,
 * before luaext_timers_leave_lua(). Closing runs <close> variables, which is
 * untrusted Lua, and the outermost leave_lua disarms the watchdog AND clears the
 * sticky interrupt flag. Sweeping after it would run those handlers unmetered
 * and uninterruptible, so `while true do end` in a <close> body would hang the
 * process -- the same denial of service luaext_timers_detach() already defends
 * close() against.
 */
void luaext_corolib_sweep(luaext_sandbox *sandbox);

#endif /* LUAEXT_COROLIB_H */
