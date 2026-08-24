/*
 * luaext — assembling the standard library a sandbox may see.
 *
 * Open into scratch, select an allow-list out, publish. The table a script can
 * reach is built here; upstream's opener only ever writes into a table that is
 * discarded before any script exists. See luaext_openlibs.h for why the
 * direction of failure on a Lua upgrade is the whole point.
 *
 * Two mechanics in here are easy to get wrong, and both are silent when wrong:
 *
 *   luaopen_base writes straight into the globals table rather than returning a
 *   fresh one, so LUA_RIDX_GLOBALS is swapped for the scratch table around every
 *   opener call. See luaext_openlibs_scratch().
 *
 *   luaopen_string points getmetatable("").__index at its OWN table, which is a
 *   second reference to the unfiltered library reachable through a path no walk
 *   of _G would ever find. See luaext_openlibs_decorate_string().
 */

#include "luaext_openlibs.h"

#include "luaext_error.h"

#include <lauxlib.h>
#include <lualib.h>

#include <string.h>

#include <Zend/zend_exceptions.h>

/* Stack slots any one step in here needs: a table, a key, a value, a spare. */
#define LUAEXT_OPENLIBS_SLOTS 8

/* -------------------------------------------------------------------------
 * The libraries built in their own translation units
 *
 * Declared here rather than in luaext_openlibs.h deliberately: that header is
 * the frozen contract three subsystems code against, and these are private
 * arrangements between this installer and the two files it drives. The debug and
 * os libraries publish themselves and so do appear in the header; base and math
 * are assembled by this file out of parts those files own.
 * ---------------------------------------------------------------------- */

extern const luaext_member luaext_baselib_allow[];
extern const char *const luaext_baselib_withheld[];
bool luaext_baselib_decorate(lua_State *L, luaext_sandbox *sandbox);

/*
 * Routes every Lua warning -- ours and the interpreter's own -- to the sandbox's
 * output policy. Installed unconditionally, so "nothing this extension runs can
 * reach stderr" is structural rather than a property of which libraries happened
 * to be opened.
 */
void luaext_baselib_install_warnf(lua_State *L, luaext_sandbox *sandbox);

extern const luaext_member luaext_mathlib_allow[];
extern const char *const luaext_mathlib_withheld[];
bool luaext_mathlib_decorate(lua_State *L, luaext_sandbox *sandbox);

/* -------------------------------------------------------------------------
 * table
 *
 * Nothing here reaches outside the interpreter and nothing here is a capability:
 * every member is bounded by the memory limit and interruptible through the
 * LUAEXT_CHECK sites patch 0004 puts in its loops. It still gets an allow-list,
 * because that is what makes an added member in some future 5.5.x fail the build
 * instead of appearing unannounced -- table.move, added in 5.3, is the reason
 * SECURITY.md tells this story at all.
 * ---------------------------------------------------------------------- */

static const luaext_member luaext_openlibs_table_allow[] = {
	{"concat", 0}, {"create", 0}, {"insert", 0}, {"move", 0}, {"pack", 0},
	{"remove", 0}, {"sort", 0},	  {"unpack", 0}, {NULL, 0},
};

static const char *const luaext_openlibs_table_withheld[] = {NULL};

/* -------------------------------------------------------------------------
 * string
 *
 * string.dump is the one member here that is a capability. It serialises a
 * function to bytecode, and Lua has no bytecode verifier: a sandbox that can
 * produce bytecode and a sandbox that can load it are one capability apart from
 * arbitrary native execution. It is reachable from every untrusted sandbox today
 * because the placeholder this file replaces opened the library wholesale.
 * ---------------------------------------------------------------------- */

static const luaext_member luaext_openlibs_string_allow[] = {
	{"byte", 0},   {"char", 0},	   {"dump", LUAEXT_CAP_DUMP_BYTECODE},
	{"find", 0},   {"format", 0},  {"gmatch", 0},
	{"gsub", 0},   {"len", 0},	   {"lower", 0},
	{"match", 0},  {"pack", 0},	   {"packsize", 0},
	{"rep", 0},	   {"reverse", 0}, {"sub", 0},
	{"unpack", 0}, {"upper", 0},   {NULL, 0},
};

static const char *const luaext_openlibs_string_withheld[] = {NULL};

/*
 * Repoint the string metatable at the table a script is allowed to see.
 *
 * luaopen_string sets getmetatable("").__index to its own table, so ("x"):dump()
 * reaches the unfiltered library without ever naming the global `string`. Every
 * method call on a string literal goes through this metatable, which makes it a
 * complete second copy of the library surface: filtering only the global would
 * withhold string.dump and leave ("").dump working.
 */
static bool luaext_openlibs_decorate_string(lua_State *L, luaext_sandbox *sandbox)
{
	(void)sandbox;

	luaL_checkstack(L, LUAEXT_OPENLIBS_SLOTS, "luaext: no stack to filter the string metatable");

	lua_pushliteral(L, "");

	if (!lua_getmetatable(L, -1)) {
		/* Only reachable if a future luaopen_string stops creating it, which
		 * would silently leave string methods unavailable on literals. */
		lua_pop(L, 1);
		luaL_error(L, "luaext: the vendored Lua no longer gives strings a metatable; "
					  "src/luaext_openlibs.c has to be revisited");
	}

	/* -3 is the selected table: the literal and its metatable sit above it. */
	lua_pushvalue(L, -3);
	lua_setfield(L, -2, "__index");
	lua_pop(L, 2);

	return true;
}

/* -------------------------------------------------------------------------
 * utf8
 * ---------------------------------------------------------------------- */

static const luaext_member luaext_openlibs_utf8_allow[] = {
	{"char", 0}, {"charpattern", 0}, {"codepoint", 0}, {"codes", 0},
	{"len", 0},	 {"offset", 0},		 {NULL, 0},
};

static const char *const luaext_openlibs_utf8_withheld[] = {NULL};

/* -------------------------------------------------------------------------
 * The registry
 * ---------------------------------------------------------------------- */

static const luaext_library luaext_openlibs_base = {
	NULL, luaopen_base, 0, luaext_baselib_allow, luaext_baselib_withheld, luaext_baselib_decorate,
};

static const luaext_library luaext_openlibs_table = {
	LUA_TABLIBNAME, luaopen_table, 0, luaext_openlibs_table_allow, luaext_openlibs_table_withheld,
	NULL,
};

static const luaext_library luaext_openlibs_string = {
	LUA_STRLIBNAME,
	luaopen_string,
	0,
	luaext_openlibs_string_allow,
	luaext_openlibs_string_withheld,
	luaext_openlibs_decorate_string,
};

static const luaext_library luaext_openlibs_math = {
	LUA_MATHLIBNAME,		 luaopen_math, 0, luaext_mathlib_allow, luaext_mathlib_withheld,
	luaext_mathlib_decorate,
};

static const luaext_library luaext_openlibs_utf8 = {
	LUA_UTF8LIBNAME,
	luaopen_utf8,
	LUAEXT_CAP_UTF8,
	luaext_openlibs_utf8_allow,
	luaext_openlibs_utf8_withheld,
	NULL,
};

/*
 * Which luaext_lib bit each library answers to, and the name its opener is
 * handed.
 *
 * The bit and the capability gate are two different questions and both are
 * asked: luaext_config_open_libs() decides whether the library is installed at
 * all, and luaext_library::require_caps decides whether this sandbox has earned
 * it. Neither is redundant -- the bits mirror Lua's own LUA_*LIBK grouping,
 * while the capabilities are ours.
 */
typedef struct {
	uint32_t bit;
	const char *modname;
	const luaext_library *library;
} luaext_openlibs_entry;

static const luaext_openlibs_entry luaext_openlibs_registry[] = {
	{LUAEXT_LIB_BASE, LUA_GNAME, &luaext_openlibs_base},
	{LUAEXT_LIB_TABLE, LUA_TABLIBNAME, &luaext_openlibs_table},
	{LUAEXT_LIB_STR, LUA_STRLIBNAME, &luaext_openlibs_string},
	{LUAEXT_LIB_MATH, LUA_MATHLIBNAME, &luaext_openlibs_math},
	{LUAEXT_LIB_UTF8, LUA_UTF8LIBNAME, &luaext_openlibs_utf8},
};

#define LUAEXT_OPENLIBS_COUNT                                                                      \
	(sizeof(luaext_openlibs_registry) / sizeof(luaext_openlibs_registry[0]))

/* -------------------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------------- */

void luaext_openlibs_scratch(lua_State *L, lua_CFunction opener, const char *name)
{
	int globals;

	luaL_checkstack(L, LUAEXT_OPENLIBS_SLOTS, "luaext: no stack to open a library");

	lua_pushglobaltable(L);
	globals = lua_gettop(L);

	/*
	 * The globals swap, and why it is safe.
	 *
	 * luaopen_base does not return a fresh table: it calls lua_pushglobaltable()
	 * and writes its members straight into whatever LUA_RIDX_GLOBALS names. So
	 * the only way to open it into scratch is to make LUA_RIDX_GLOBALS *be*
	 * scratch for the duration of the call.
	 *
	 * Nothing observes the swap. No Lua code runs between the two assignments --
	 * an opener is a C function and none of them call back into a script -- and
	 * no chunk exists yet to hold an _ENV upvalue: lua_load() copies
	 * LUA_RIDX_GLOBALS into a main chunk's first upvalue at COMPILE time, and the
	 * sandbox has compiled nothing when this runs. A chunk compiled while the
	 * swap was in place would run against the scratch table and see an empty
	 * world, which is exactly why this must never be reached from anywhere but
	 * construction.
	 *
	 * Applied to every opener, not just luaopen_base. It costs nothing and it
	 * means a future upstream library that decides to publish a global cannot do
	 * it behind this file's back.
	 */
	lua_createtable(L, 0, 32);
	lua_pushvalue(L, -1);
	lua_rawseti(L, LUA_REGISTRYINDEX, LUA_RIDX_GLOBALS);

	lua_pushcfunction(L, opener);
	lua_pushstring(L, name);

	/*
	 * Deliberately not luaL_requiref(): that writes _LOADED[name] and _G[name],
	 * publishing the unfiltered table under both -- and _LOADED becomes
	 * package.loaded the moment require() lands.
	 *
	 * Unprotected on purpose. The installer's own lua_pcall is what catches a
	 * memory error here, and restoring LUA_RIDX_GLOBALS on that path is its job:
	 * it holds the real globals table in a stack slot underneath the call
	 * precisely so an unwind cannot lose it.
	 */
	lua_call(L, 1, 1);

	lua_pushvalue(L, globals);
	lua_rawseti(L, LUA_REGISTRYINDEX, LUA_RIDX_GLOBALS);

	/*
	 * An opener that returned its own fresh table (every one but base) leaves the
	 * table we pre-created unused; base returns the swapped-in globals, which IS
	 * that table. Either way what stays is the opener's answer.
	 */
	lua_remove(L, -2);
	lua_remove(L, globals);
}

int luaext_openlibs_select(lua_State *L, luaext_sandbox *sandbox, int scratch_index,
						   const luaext_member *allow)
{
	int listed = 0;
	int selected = 0;
	int index;

	scratch_index = lua_absindex(L, scratch_index);

	while (allow[listed].name != NULL) {
		listed++;
	}

	luaL_checkstack(L, LUAEXT_OPENLIBS_SLOTS, "luaext: no stack to select a library's members");
	lua_createtable(L, 0, listed);

	for (index = 0; index < listed; index++) {
		const luaext_member *member = &allow[index];

		lua_pushstring(L, member->name);
		lua_rawget(L, scratch_index);

		/*
		 * Checked before the capability, so the check still runs for a member
		 * this sandbox is not getting. A name that has been renamed or removed
		 * upstream would otherwise only be noticed by whoever happened to grant
		 * the capability that selects it.
		 */
		if (lua_isnil(L, -1)) {
			lua_pop(L, 1);
			luaL_error(L,
					   "luaext: the vendored Lua has no \"%s\" to select; the allow list in src/ "
					   "is describing a library that no longer exists",
					   member->name);
		}

		if (member->require_caps != 0 &&
			(sandbox->policy.caps & member->require_caps) != member->require_caps) {
			lua_pop(L, 1);
			continue;
		}

		lua_pushstring(L, member->name);
		lua_insert(L, -2);
		lua_rawset(L, -3);
		selected++;
	}

	return selected;
}

static bool luaext_openlibs_listed(const luaext_member *allow, const char *const *withheld,
								   const char *name)
{
	int index;

	for (index = 0; allow[index].name != NULL; index++) {
		if (strcmp(allow[index].name, name) == 0) {
			return true;
		}
	}

	for (index = 0; withheld[index] != NULL; index++) {
		if (strcmp(withheld[index], name) == 0) {
			return true;
		}
	}

	return false;
}

void luaext_openlibs_check_drift(lua_State *L, int scratch_index, const luaext_member *allow,
								 const char *const *withheld, const char *library)
{
	scratch_index = lua_absindex(L, scratch_index);

	luaL_checkstack(L, LUAEXT_OPENLIBS_SLOTS, "luaext: no stack to check a library for drift");

	lua_pushnil(L);

	while (lua_next(L, scratch_index) != 0) {
		lua_pop(L, 1); /* the value; the key stays, for the next lua_next */

		/*
		 * Read only after the type check: lua_tostring() on a number key would
		 * rewrite it in place and corrupt the traversal.
		 */
		if (lua_type(L, -1) != LUA_TSTRING) {
			continue;
		}

		if (!luaext_openlibs_listed(allow, withheld, lua_tostring(L, -1))) {
			luaL_error(L,
					   "luaext: the vendored Lua's \"%s\" library has a member \"%s\" that is "
					   "neither allowed nor withheld. Classify it in src/luaext_openlibs.c (or the "
					   "file that owns this library's lists) before shipping the upgrade",
					   library, lua_tostring(L, -1));
		}
	}
}

/* -------------------------------------------------------------------------
 * Publishing
 * ---------------------------------------------------------------------- */

/*
 * Copy the selected members into the globals table.
 *
 * Only the base library takes this path; every other library is published as one
 * named global. `_G` needs fixing up afterwards because luaopen_base pointed it
 * at whatever the globals table was when it ran, which was scratch.
 */
static void luaext_openlibs_publish_globals(lua_State *L, int selected_index)
{
	int globals;

	luaL_checkstack(L, LUAEXT_OPENLIBS_SLOTS, "luaext: no stack to publish the base library");

	selected_index = lua_absindex(L, selected_index);

	lua_pushglobaltable(L);
	globals = lua_gettop(L);

	lua_pushnil(L);

	while (lua_next(L, selected_index) != 0) {
		/* key, value -- and rawset wants them the other way up from a copy of
		 * the key, because lua_next needs its own key left in place. */
		lua_pushvalue(L, -2);
		lua_insert(L, -2);
		lua_rawset(L, globals);
	}

	/*
	 * _G, which luaopen_base pointed at whatever the globals table was when it
	 * ran -- the scratch table. Left alone it would be a live reference to the
	 * unfiltered base library sitting in plain sight under its most obvious name.
	 */
	lua_pushliteral(L, LUA_GNAME);
	lua_pushvalue(L, globals);
	lua_rawset(L, globals);

	lua_pop(L, 1);
}

static void luaext_openlibs_publish_named(lua_State *L, const char *global, int selected_index)
{
	luaL_checkstack(L, LUAEXT_OPENLIBS_SLOTS, "luaext: no stack to publish a library");

	selected_index = lua_absindex(L, selected_index);

	lua_pushglobaltable(L);
	lua_pushstring(L, global);
	lua_pushvalue(L, selected_index);
	lua_rawset(L, -3);
	lua_pop(L, 1);
}

/* -------------------------------------------------------------------------
 * The installer
 * ---------------------------------------------------------------------- */

/*
 * Build every library, under the caller's lua_pcall.
 *
 * Argument 1: the sandbox.
 */
static int luaext_openlibs_build(lua_State *L)
{
	luaext_sandbox *sandbox = (luaext_sandbox *)lua_touserdata(L, 1);
	size_t index;

	lua_settop(L, 0);
	luaL_checkstack(L, LUAEXT_OPENLIBS_SLOTS, "luaext: no stack to install the standard library");

	for (index = 0; index < LUAEXT_OPENLIBS_COUNT; index++) {
		const luaext_openlibs_entry *entry = &luaext_openlibs_registry[index];
		const luaext_library *library = entry->library;
		int selected;

		if ((sandbox->policy.open_libs & entry->bit) == 0) {
			continue;
		}

		if (library->require_caps != 0 &&
			(sandbox->policy.caps & library->require_caps) != library->require_caps) {
			continue;
		}

		luaext_openlibs_scratch(L, library->opener, entry->modname);
		luaext_openlibs_check_drift(L, -1, library->allow, library->withheld,
									library->global != NULL ? library->global : LUA_GNAME);

		selected = luaext_openlibs_select(L, sandbox, -1, library->allow);

		/*
		 * Nothing survived selection, so there is nothing to publish. An empty
		 * table would be worse than absence: `if string then` would answer yes to
		 * a script and every call through it would then fail one by one.
		 */
		if (selected == 0) {
			lua_pop(L, 2);
			continue;
		}

		if (library->decorate != NULL && !library->decorate(L, sandbox)) {
			luaL_error(L, "luaext: the \"%s\" library could not be assembled",
					   library->global != NULL ? library->global : LUA_GNAME);
		}

		if (library->global == NULL) {
			luaext_openlibs_publish_globals(L, -1);
		} else {
			luaext_openlibs_publish_named(L, library->global, -1);
		}

		/* The scratch table is unreachable from here on, and collectable. */
		lua_pop(L, 2);
	}

	if (!luaext_debuglib_install(L, sandbox)) {
		luaL_error(L, "luaext: the \"debug\" library could not be assembled");
	}

	if (!luaext_oslib_install(L, sandbox)) {
		luaL_error(L, "luaext: the \"os\" library could not be assembled");
	}

	/*
	 * Every scratch table is unreachable by now, and one of them is a complete
	 * copy of the base library. Collecting here rather than leaving it to the
	 * first collection a script happens to trigger keeps a freshly constructed
	 * sandbox's reported memory honest -- getMemoryUsage() should describe what
	 * the sandbox holds, not what assembling it passed through.
	 */
	lua_gc(L, LUA_GCCOLLECT);

	return 0;
}

bool luaext_openlibs_install(luaext_sandbox *sandbox)
{
	lua_State *L = sandbox->L;
	int globals;
	int status;

	if (!lua_checkstack(L, LUAEXT_OPENLIBS_SLOTS)) {
		zend_throw_exception(
			luaext_ce_memory_limit_error,
			"Cannot install the standard library: the interpreter stack cannot grow", 0);
		return false;
	}

	/*
	 * Unconditional, and before the libraries: a sandbox that opens no library at
	 * all still must not let the interpreter's own diagnostics -- a failing
	 * finaliser, most often -- reach the process's stderr.
	 */
	luaext_baselib_install_warnf(L, sandbox);

	/*
	 * The real globals table, parked below the protected call.
	 *
	 * luaext_openlibs_scratch() swaps LUA_RIDX_GLOBALS while an opener runs, and
	 * an error raised mid-swap unwinds past its restore. lua_pcall does not touch
	 * stack slots below the function it called, so this slot survives any failure
	 * and the restore below is unconditional -- a half-installed sandbox is
	 * abandoned by the caller either way, but not one whose globals table is a
	 * scratch table the destructor will walk.
	 */
	lua_pushglobaltable(L);
	globals = lua_gettop(L);

	lua_pushcfunction(L, luaext_openlibs_build);
	lua_pushlightuserdata(L, sandbox);

	status = lua_pcall(L, 1, 0, 0);

	lua_pushvalue(L, globals);
	lua_rawseti(L, LUA_REGISTRYINDEX, LUA_RIDX_GLOBALS);

	if (status != LUA_OK) {
		const char *message = lua_tostring(L, -1);

		zend_throw_exception_ex(status == LUA_ERRMEM ? luaext_ce_memory_limit_error
													 : luaext_ce_configuration_error,
								0, "Cannot install the standard library: %s",
								message != NULL ? message : "the interpreter ran out of memory");

		lua_pop(L, 1);
	}

	lua_remove(L, globals);

	return status == LUA_OK;
}
