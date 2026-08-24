/*
 * luaext — the base library a sandbox sees.
 *
 * SCAFFOLD. Owns the replacements for pcall, xpcall, print, collectgarbage,
 * load and warn, plus the warning hook. Nothing is implemented yet.
 *
 * pcall/xpcall are the linchpin of the whole extension: a limit that a script
 * can catch is not a limit. Two things the implementation must get right, both
 * of which are easy to miss:
 *
 *   The test is on the lua_pcallk STATUS as well as the error value. A refused
 *   allocation raises LUA_ERRMEM carrying Lua's own preallocated string, not
 *   our userdata, so luaext_error_is_fatal() cannot see it -- and without the
 *   status check a script can pcall its way straight past memoryBytes.
 *
 *   xpcall's message handler is SKIPPED for a fatal, not run-then-rethrown. A
 *   handler's return value becomes the error object, so one line of Lua
 *   (`xpcall(f, function() return "oops" end)`) would replace the unforgeable
 *   marker with a plain string that every outer pcall then treats as catchable.
 */

#include "luaext_openlibs.h"

#include <lauxlib.h>
#include <lualib.h>
