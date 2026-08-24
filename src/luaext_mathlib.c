/*
 * luaext — the math library a sandbox sees.
 *
 * Two jobs, and the second is the one that is easy to overlook:
 *
 *   Replace math.randomseed so it returns nothing. Upstream 5.4+ returns the
 *   seed components it derived, and those are partly address-based -- a
 *   straight ASLR disclosure.
 *
 *   RESEED the generator at install time from the sandbox's own seed. Upstream
 *   seeds it when the library opens, from a stack address mixed with a
 *   second-granularity clock, so a script that calls math.random(0) a few times
 *   can brute-force the state offline. Replacing the function does nothing
 *   about that; only reseeding does.
 *
 * Both go through the upstream closure rather than touching the generator's
 * state directly: RanState is private to lmathlib.c, and the closure is right
 * there in the scratch table with the state as its upvalue.
 */

#include "luaext_openlibs.h"

#include <lauxlib.h>
#include <lualib.h>

#include <ext/random/php_random.h>
#include <ext/random/php_random_csprng.h>

/* Stack slots any one step in here needs. */
#define LUAEXT_MATHLIB_SLOTS 8

/*
 * Nothing in the math library reaches outside the interpreter, so nothing here
 * is capability-gated; the allow list exists so that a member added by a future
 * 5.5.x fails the drift check rather than appearing unannounced.
 *
 * The upstream table also carries `random`, which is deliberately NOT replaced:
 * it is the same closure the reseed below acts on, and once the generator is
 * seeded from the sandbox's own entropy there is nothing left in it to fix.
 */
const luaext_member luaext_mathlib_allow[] = {
	{"abs", 0},		  {"acos", 0},	  {"asin", 0},		 {"atan", 0},		{"ceil", 0},
	{"cos", 0},		  {"deg", 0},	  {"exp", 0},		 {"floor", 0},		{"fmod", 0},
	{"frexp", 0},	  {"huge", 0},	  {"ldexp", 0},		 {"log", 0},		{"max", 0},
	{"maxinteger", 0}, {"min", 0},	  {"mininteger", 0}, {"modf", 0},		{"pi", 0},
	{"rad", 0},		  {"random", 0},  {"randomseed", 0}, {"sin", 0},		{"sqrt", 0},
	{"tan", 0},		  {"tointeger", 0}, {"type", 0},	 {"ult", 0},		{NULL, 0},
};

const char *const luaext_mathlib_withheld[] = {NULL};

/*
 * Reinterpret a 64-bit seed component as a lua_Integer.
 *
 * Spelled out rather than cast, because converting an out-of-range unsigned to a
 * signed type is implementation-defined, and a seed that came out differently on
 * one compiler would make `deterministic: true` quietly untrue there.
 */
static lua_Integer luaext_mathlib_as_integer(uint64_t value)
{
	if (value <= (uint64_t)LUA_MAXINTEGER) {
		return (lua_Integer)value;
	}

	return (lua_Integer)(value - (uint64_t)LUA_MAXINTEGER - 1) + LUA_MININTEGER;
}

/*
 * The two components the generator is seeded with.
 *
 * A host that pinned SandboxConfig::$seed gets a reproducible sequence, which is
 * what `deterministic: true` promises. Everything else draws from the CSPRNG:
 * upstream's own seeding is a stack address mixed with a clock of one-second
 * resolution, which is guessable offline from a handful of math.random(0) calls.
 */
static void luaext_mathlib_seed(const luaext_sandbox *sandbox, uint64_t parts[2])
{
	parts[0] = 0;
	parts[1] = 0;

	if (sandbox->policy.seed_is_fixed) {
		parts[0] = sandbox->policy.seed;

		/* Derived rather than zero, so a fixed seed still spreads across the
		 * generator's state the way a random pair would. */
		parts[1] = sandbox->policy.seed ^ UINT64_C(0x9E3779B97F4A7C15);
		return;
	}

	if (php_random_bytes_silent(parts, sizeof(uint64_t) * 2) == FAILURE) {
		parts[0] = (uint64_t)php_random_generate_fallback_seed();
		parts[1] = (uint64_t)php_random_generate_fallback_seed();
	}
}

/*
 * Upvalue 1 is upstream's randomseed.
 *
 * Void, and integer-only. Upstream returns the two components it used, which for
 * a no-argument call are derived from luaL_makeseed() -- a stack address and a
 * clock. Returning them hands a script the address; accepting the no-argument
 * form at all would reseed from the address again. Both are refused here, and a
 * script that wants a fresh sequence supplies its own number.
 */
static int luaext_mathlib_randomseed(lua_State *L)
{
	lua_Integer first = luaL_checkinteger(L, 1);
	lua_Integer second = luaL_optinteger(L, 2, 0);

	luaL_checkstack(L, LUAEXT_MATHLIB_SLOTS, "luaext: no stack to reseed");

	lua_settop(L, 0);
	lua_pushvalue(L, lua_upvalueindex(1));
	lua_pushinteger(L, first);
	lua_pushinteger(L, second);
	lua_call(L, 2, 0);

	return 0;
}

bool luaext_mathlib_decorate(lua_State *L, luaext_sandbox *sandbox)
{
	int scratch = lua_absindex(L, -2);
	int selected = lua_absindex(L, -1);
	uint64_t parts[2];

	luaL_checkstack(L, LUAEXT_MATHLIB_SLOTS, "luaext: no stack to assemble the math library");

	/*
	 * The reseed, through the upstream closure while it is still reachable. It
	 * shares its RanState upvalue with math.random, which is what makes this
	 * reach the generator a script will actually draw from.
	 */
	luaext_mathlib_seed(sandbox, parts);

	lua_pushliteral(L, "randomseed");
	lua_rawget(L, scratch);
	lua_pushinteger(L, luaext_mathlib_as_integer(parts[0]));
	lua_pushinteger(L, luaext_mathlib_as_integer(parts[1]));
	lua_call(L, 2, 0);

	lua_pushliteral(L, "randomseed");
	lua_rawget(L, scratch);
	lua_pushcclosure(L, luaext_mathlib_randomseed, 1);

	lua_pushliteral(L, "randomseed");
	lua_insert(L, -2);
	lua_rawset(L, selected);

	return true;
}
