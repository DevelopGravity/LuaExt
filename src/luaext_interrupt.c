/*
 * luaext — interrupt delivery into the interpreter.
 *
 * The patched loops in the vendored Lua tree call LUAEXT_CHECK(), which lands
 * here once the watchdog has raised the interrupt flag. The declaration lives
 * in third_party/lua-5.5.1/src/luaext_lua_hooks.h, which deliberately knows
 * nothing about PHP; this is the only definition in the extension.
 */

#include "luaext_types.h"

#include <lauxlib.h>
#include <lua.h>

/*
 * Stop the running script.
 *
 * TODO: raise the unforgeable fatal-error userdata carrying the reason from
 * luaext_irq::reason (CPU limit, wall clock, output budget or host abort) so
 * that the sandbox's pcall replacement can refuse to let a script catch it, and
 * so the host receives the matching typed exception. Until that machinery
 * exists this raises an ordinary Lua error, which a script *can* catch -- which
 * is precisely why no limit is reported as enforced by Sandbox::features().
 */
void luaext_raise_interrupt(lua_State *L)
{
	(void)luaL_error(L, "luaext: execution interrupted");
}
