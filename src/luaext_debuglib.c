/*
 * luaext — the debug library a sandbox sees.
 *
 * SCAFFOLD. Publishes nothing yet, which is the safe direction: today no debug
 * table exists at all, so this stub preserves that.
 *
 * When it is built, three placements matter more than they look:
 *
 *   debug.getmetatable and debug.setmetatable belong to debugMutate, NOT
 *   debugIntrospect, despite reading like introspection. They bypass
 *   __metatable, which is the entire protection that makes an error value
 *   opaque to a script -- and Capabilities::trusted() grants debugIntrospect,
 *   so misfiling them would quietly void that guarantee for every trusted
 *   sandbox.
 *
 *   getupvalue, setupvalue, upvalueid and upvaluejoin must refuse a C function.
 *   The PHP-callback closure carries its zend_fcall_info_cache as upvalue 1, so
 *   an unguarded getupvalue hands a script the host-callable storage of every
 *   registered function, and setupvalue lets it swap one host function's
 *   callable into another's closure.
 *
 *   debug.debug is withheld at every capability level. It is an interactive
 *   REPL that reads from stdin.
 */

#include "luaext_openlibs.h"

#include <lauxlib.h>
#include <lualib.h>

bool luaext_debuglib_install(lua_State *L, luaext_sandbox *sandbox)
{
	(void)L;
	(void)sandbox;
	return true;
}
