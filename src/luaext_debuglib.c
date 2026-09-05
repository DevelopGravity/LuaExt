/*
 * luaext — the debug library a sandbox sees.
 *
 * Built by selecting out of a scratch-opened luaopen_debug, never by publishing
 * upstream's table. With no debug capability at all nothing is published: the
 * global must be nil rather than an empty table, so `if debug then` in a script
 * answers the question it looks like it is asking.
 *
 * Three placements matter more than they look:
 *
 *   debug.getmetatable and debug.setmetatable belong to debugMutate, NOT
 *   debugIntrospect, despite reading like introspection. They bypass
 *   __metatable, which is the entire protection that makes an error value
 *   opaque to a script -- and Capabilities::trusted() grants debugIntrospect,
 *   so misfiling them would quietly void that guarantee for every trusted
 *   sandbox. tests/03-adversarial/error-value-opaque-to-lua.phpt is what would
 *   start failing, and only for a preset nothing in that test constructs.
 *
 *   getupvalue, setupvalue, upvalueid and upvaluejoin must refuse a C function.
 *   The PHP-callback closure carries its zend_fcall_info_cache as upvalue 1
 *   (see luaext_phpcall_build_closure), so an unguarded getupvalue hands a
 *   script the host-callable storage of every registered function, and
 *   setupvalue lets it swap one host function's callable into another's
 *   closure. math.random's RanState upvalue is covered by the same refusal.
 *
 *   debug.debug is withheld at every capability level. It is an interactive
 *   REPL that reads from stdin.
 *
 * On debugMutate generally: granting it means the sandbox stops being a
 * sandbox. debug.getregistry alone hands a script LUA_REGISTRYINDEX, which
 * holds the error metatable, the refs table behind every LuaFunction handle,
 * and _LOADED. From there a script can forge a fatal error or strip the marker
 * off one, and a limit a script can forge or catch is not a limit. The
 * capability exists for host-side debugging tools, not for running code you did
 * not write.
 */

#include "luaext_openlibs.h"

#include <lauxlib.h>
#include <lualib.h>

/* Any one of these means the table gets published; none means it stays nil. */
#define LUAEXT_DEBUGLIB_CAPS                                                                       \
	(LUAEXT_CAP_DEBUG_TRACEBACK | LUAEXT_CAP_DEBUG_INTROSPECT | LUAEXT_CAP_DEBUG_MUTATE |          \
	 LUAEXT_CAP_DEBUG_HOOKS)

/* -------------------------------------------------------------------------
 * Membership
 * ---------------------------------------------------------------------- */

static const luaext_member luaext_debuglib_allow[] = {
	/* Reads only its own stack. Safe for the untrusted baseline. */
	{"traceback", LUAEXT_CAP_DEBUG_TRACEBACK},

	/* Discloses ar.source -- the full text of every chunk in the sandbox,
	 * including ones the host loaded and never meant a script to read. That is
	 * intra-sandbox disclosure only, which is why it is a capability rather
	 * than a refusal. */
	{"getinfo", LUAEXT_CAP_DEBUG_INTROSPECT},

	/* Reads the locals of frames the script does not own. */
	{"getlocal", LUAEXT_CAP_DEBUG_INTROSPECT},
	{"getupvalue", LUAEXT_CAP_DEBUG_INTROSPECT},

	{"setlocal", LUAEXT_CAP_DEBUG_MUTATE},

	/* Rewrites _ENV, among everything else. */
	{"setupvalue", LUAEXT_CAP_DEBUG_MUTATE},

	/*
	 * upvalueid returns a light userdata that IS the raw upvalue address. It is
	 * non-leaky only because two vendored patches hide addresses from Lua:
	 * patches/0006 removes them from tostring() and %s stringification, and
	 * patches/0003 rejects string.format("%p"). If EITHER patch is ever
	 * dropped, these two members go with it -- there would be no way left to
	 * hold an upvalue id without being able to read the address out of it.
	 */
	{"upvalueid", LUAEXT_CAP_DEBUG_MUTATE},
	{"upvaluejoin", LUAEXT_CAP_DEBUG_MUTATE},

	/* Hands over LUA_REGISTRYINDEX. See the file header. */
	{"getregistry", LUAEXT_CAP_DEBUG_MUTATE},

	/* debugMutate, not debugIntrospect: these bypass __metatable. */
	{"getmetatable", LUAEXT_CAP_DEBUG_MUTATE},
	{"setmetatable", LUAEXT_CAP_DEBUG_MUTATE},

	/* setuservalue overwrites the structured traceback on an error userdata. */
	{"getuservalue", LUAEXT_CAP_DEBUG_MUTATE},
	{"setuservalue", LUAEXT_CAP_DEBUG_MUTATE},

	/* sethook displaces the always-armed count hook the CPU limit rides on,
	 * which is why luaext_config_resolve refuses debugHooks together with a CPU
	 * limit rather than letting the limit quietly stop meaning anything. */
	{"sethook", LUAEXT_CAP_DEBUG_HOOKS},
	{"gethook", LUAEXT_CAP_DEBUG_HOOKS},

	{NULL, 0},
};

/* An interactive REPL reading stdin. Never appropriate, at any level. */
static const char *const luaext_debuglib_withheld[] = {
	"debug",
	NULL,
};

/* -------------------------------------------------------------------------
 * Refusing to treat a C function's upvalues as script state
 * ---------------------------------------------------------------------- */

/*
 * The members that reach into a closure's upvalues, and where they take a
 * closure argument. Upstream's own signatures: none of these accepts an
 * optional leading thread, so argument 1 is always a closure, and upvaluejoin
 * takes a second one at argument 3.
 */
typedef struct {
	const char *name;
	int second_closure_arg; /* 0 when the member takes only one closure */
} luaext_debuglib_guarded;

static const luaext_debuglib_guarded luaext_debuglib_guarded_members[] = {
	{"getupvalue", 0},
	{"setupvalue", 0},
	{"upvalueid", 0},
	{"upvaluejoin", 3},
};

/*
 * upvalue 1: the upstream implementation lifted out of scratch
 * upvalue 2: the member name, for the message
 * upvalue 3: the second closure argument's index, or 0
 */
static int luaext_debuglib_refuse_cfunction(lua_State *L)
{
	const char *name = lua_tostring(L, lua_upvalueindex(2));
	int second = (int)lua_tointeger(L, lua_upvalueindex(3));
	int argc = lua_gettop(L);

	if (lua_iscfunction(L, 1)) {
		return luaL_error(L, "debug.%s: a C function's upvalues are host state, not script state",
						  name);
	}

	if (second != 0 && lua_iscfunction(L, second)) {
		return luaL_error(L, "debug.%s: a C function's upvalues are host state, not script state",
						  name);
	}

	/*
	 * Everything else is upstream's, unchanged: this wrapper adds a refusal and
	 * takes nothing away, so a Lua closure behaves exactly as the manual says.
	 */
	luaL_checkstack(L, 1, "luaext: no stack to call the debug library");
	lua_pushvalue(L, lua_upvalueindex(1));
	lua_insert(L, 1);
	lua_call(L, argc, LUA_MULTRET);

	return lua_gettop(L);
}

/*
 * Replace each selected upvalue member with a closure over the original.
 *
 * A member absent from `selected` simply had its capability withheld; there is
 * nothing to guard and nothing to report.
 */
static void luaext_debuglib_guard_upvalue_members(lua_State *L, int selected)
{
	size_t index;

	for (index = 0; index < sizeof(luaext_debuglib_guarded_members) /
								sizeof(luaext_debuglib_guarded_members[0]);
		 index++) {
		const luaext_debuglib_guarded *member = &luaext_debuglib_guarded_members[index];

		lua_pushstring(L, member->name);
		lua_rawget(L, selected);

		if (lua_isnil(L, -1)) {
			lua_pop(L, 1);
			continue;
		}

		lua_pushstring(L, member->name);
		lua_pushinteger(L, (lua_Integer)member->second_closure_arg);
		lua_pushcclosure(L, luaext_debuglib_refuse_cfunction, 3);

		lua_pushstring(L, member->name);
		lua_insert(L, -2);
		lua_rawset(L, selected);
	}
}

/* -------------------------------------------------------------------------
 * Installation
 * ---------------------------------------------------------------------- */

bool luaext_debuglib_install(lua_State *L, luaext_sandbox *sandbox)
{
	int scratch;
	int selected;

	/*
	 * No debug capability at all: publish nothing, and do not even open the
	 * upstream library to drift-check it. The drift check still runs for every
	 * sandbox that has any debug capability -- which includes the untrusted
	 * baseline, since debugTraceback is part of it -- so an upstream member
	 * added by a point release cannot reach a real sandbox unclassified.
	 */
	if ((sandbox->policy.caps & LUAEXT_DEBUGLIB_CAPS) == 0) {
		return true;
	}

	luaL_checkstack(L, 8, "luaext: no stack to build the debug library");

	luaext_openlibs_scratch(L, luaopen_debug, LUA_DBLIBNAME);
	scratch = lua_gettop(L);

	luaext_openlibs_check_drift(L, scratch, luaext_debuglib_allow, luaext_debuglib_withheld,
								LUA_DBLIBNAME);

	if (luaext_openlibs_select(L, sandbox, scratch, luaext_debuglib_allow, "debug") == 0) {
		/* Every granted capability selected nothing, which can only mean the
		 * allow list and the capability set have drifted apart. Publishing {}
		 * would be worse than publishing nothing. */
		lua_pop(L, 2);
		return true;
	}

	selected = lua_gettop(L);
	luaext_debuglib_guard_upvalue_members(L, selected);

	lua_setglobal(L, LUA_DBLIBNAME); /* pops the selected table */
	lua_pop(L, 1);					 /* pops scratch */

	return true;
}
