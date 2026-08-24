/*
 * luaext — assembling the standard library a sandbox may see.
 *
 * SCAFFOLD. This currently reproduces the Wave-1 placeholder behaviour exactly,
 * moved here out of luaext_sandbox.c so that the library-policy work lands in
 * this file and nowhere else. It opens upstream libraries wholesale and then
 * deletes four base names -- which is a deny-list, and is the thing the header
 * explains at length that we must stop doing.
 *
 * What has to replace it: open into scratch, select an allow-list out, publish.
 * See luaext_openlibs.h.
 */

#include "luaext_openlibs.h"

#include <lauxlib.h>
#include <lualib.h>

/*
 * Which upstream libraries the placeholder opens.
 *
 * LUAEXT_LIB_CORO has no entry and must not gain one: upstream's coroutine
 * library caps nothing and lets resume swallow a fatal error, so it is only
 * ever installed through our own wrapper. That wrapper does not exist yet,
 * which is why luaext_config_open_libs() no longer sets the bit.
 */
typedef struct {
	uint32_t bit;
	const char *name;
	lua_CFunction open;
} luaext_placeholder_library;

static const luaext_placeholder_library luaext_placeholder_libraries[] = {
	{LUAEXT_LIB_BASE, LUA_GNAME, luaopen_base},
	{LUAEXT_LIB_TABLE, LUA_TABLIBNAME, luaopen_table},
	{LUAEXT_LIB_STR, LUA_STRLIBNAME, luaopen_string},
	{LUAEXT_LIB_MATH, LUA_MATHLIBNAME, luaopen_math},
	{LUAEXT_LIB_UTF8, LUA_UTF8LIBNAME, luaopen_utf8},
};

/*
 * Base-library members that reach outside the sandbox: dofile() and loadfile()
 * open real files, load() is the compileAtRuntime capability, and warn()
 * reaches stderr through Lua's default warning function.
 *
 * Deleting them afterwards is the deny-list this file exists to replace, and it
 * is also why granting compileAtRuntime or warn currently does nothing at all.
 */
static const char *const luaext_placeholder_removals[] = {
	"dofile",
	"loadfile",
	"load",
	"warn",
};

bool luaext_openlibs_install(luaext_sandbox *sandbox)
{
	lua_State *L = sandbox->L;
	size_t index;

	for (index = 0;
		 index < sizeof(luaext_placeholder_libraries) / sizeof(luaext_placeholder_libraries[0]);
		 index++) {
		const luaext_placeholder_library *library = &luaext_placeholder_libraries[index];

		if ((sandbox->policy.open_libs & library->bit) == 0) {
			continue;
		}

		luaL_requiref(L, library->name, library->open, 1);
		lua_pop(L, 1);
	}

	if ((sandbox->policy.open_libs & LUAEXT_LIB_BASE) != 0) {
		lua_pushglobaltable(L);

		for (index = 0;
			 index < sizeof(luaext_placeholder_removals) / sizeof(luaext_placeholder_removals[0]);
			 index++) {
			lua_pushnil(L);
			lua_setfield(L, -2, luaext_placeholder_removals[index]);
		}

		lua_pop(L, 1);
	}

	return true;
}

/* -------------------------------------------------------------------------
 * Helpers the per-library builders use. Scaffold stubs.
 * ---------------------------------------------------------------------- */

void luaext_openlibs_scratch(lua_State *L, lua_CFunction opener, const char *name)
{
	(void)L;
	(void)opener;
	(void)name;
}

int luaext_openlibs_select(lua_State *L, luaext_sandbox *sandbox, int scratch_index,
						   const luaext_member *allow)
{
	(void)L;
	(void)sandbox;
	(void)scratch_index;
	(void)allow;
	return 0;
}

void luaext_openlibs_check_drift(lua_State *L, int scratch_index, const luaext_member *allow,
								 const char *const *withheld, const char *library)
{
	(void)L;
	(void)scratch_index;
	(void)allow;
	(void)withheld;
	(void)library;
}
