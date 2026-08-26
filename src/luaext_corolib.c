/*
 * luaext — the coroutine library, wrapped. See luaext_corolib.h for why.
 */

#include "luaext_corolib.h"

#include "luaext_error.h"

#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>

/* -------------------------------------------------------------------------
 * Live-thread tracking
 *
 * A weak-KEYED registry table holding every coroutine this sandbox created.
 * Weak so a thread the collector reclaims leaves on its own; the sweep and the
 * cap both read it.
 * ---------------------------------------------------------------------- */

static void luaext_corolib_push_threads(lua_State *L)
{
	if (lua_rawgetp(L, LUA_REGISTRYINDEX, &luaext_key_threads) == LUA_TTABLE) {
		return;
	}

	lua_pop(L, 1);
	lua_createtable(L, 0, 8);

	/* Weak keys: a collected coroutine drops out without anyone maintaining it.
	 * Its value is `true` and carries nothing, so weak values would be wrong --
	 * the key is the thread. */
	lua_createtable(L, 0, 1);
	lua_pushliteral(L, "k");
	lua_setfield(L, -2, "__mode");
	lua_setmetatable(L, -2);

	lua_pushvalue(L, -1);
	lua_rawsetp(L, LUA_REGISTRYINDEX, &luaext_key_threads);
}

/*
 * Recount the table, which is only ever done after a full collection.
 *
 * co_live is a running counter and is exact for threads this sandbox is still
 * tracking, but a coroutine that finished and became garbage stays counted until
 * something collects it. Recounting is how the cap distinguishes "sixty-four
 * live" from "sixty-four created, most of them dead".
 */
static uint32_t luaext_corolib_recount(lua_State *L)
{
	uint32_t live = 0;

	luaext_corolib_push_threads(L);

	lua_pushnil(L);

	while (lua_next(L, -2) != 0) {
		lua_pop(L, 1); /* value; the key stays for lua_next */
		live++;
	}

	lua_pop(L, 1);

	return live;
}

/* -------------------------------------------------------------------------
 * create
 * ---------------------------------------------------------------------- */

static int luaext_corolib_create(lua_State *L)
{
	luaext_sandbox *sandbox = LUAEXT_SB(L);
	uint32_t cap;
	lua_State *co;

	luaL_checktype(L, 1, LUA_TFUNCTION);

	cap = sandbox->policy.limits.max_live_coroutines;

	if (cap != 0 && sandbox->co_live >= cap) {
		/*
		 * A collection before the refusal, not after. Most programs that reach
		 * the cap have simply left finished coroutines lying around, and
		 * refusing those would make the limit describe allocation history rather
		 * than what is alive. Only when a real collection cannot bring the count
		 * down does this fail.
		 */
		lua_gc(L, LUA_GCCOLLECT);
		sandbox->co_live = luaext_corolib_recount(L);

		if (sandbox->co_live >= cap) {
			/*
			 * Fatal, not a catchable error. A script that could pcall this would
			 * retry in a loop, and the cap exists to bound the interpreter's
			 * memory, so letting the script decide to ignore it would defeat it.
			 */
			luaext_error_raise(L, LUAEXT_ERR_COROUTINE, true,
							   "The sandbox already has %u live coroutine(s), which is its "
							   "Limits::$maxLiveCoroutines",
							   (unsigned int)cap);
		}
	}

	co = lua_newthread(L);

	/* The body goes onto the new thread's stack, where the first resume finds
	 * it. lua_xmove moves rather than copies, so nothing is left behind. */
	lua_pushvalue(L, 1);
	lua_xmove(L, co, 1);

	luaext_corolib_push_threads(L);
	lua_pushvalue(L, -2); /* the thread, as the key */
	lua_pushboolean(L, 1);
	lua_rawset(L, -3);
	lua_pop(L, 1);

	sandbox->co_live++;

	return 1;
}

/* -------------------------------------------------------------------------
 * resume
 * ---------------------------------------------------------------------- */

/*
 * Move the error on `from`'s stack top to `to`, and raise it there as a fatal.
 *
 * The status check is the whole point, and it is the same trap pcall documents:
 * a refused allocation raises LUA_ERRMEM carrying Lua's own preallocated string
 * rather than our unforgeable marker, so an error-value test alone would let a
 * script move its allocation into a coroutine and resume straight past
 * Limits::$memoryBytes.
 */
static bool luaext_corolib_is_fatal(lua_State *co, int status)
{
	if (status == LUA_ERRMEM) {
		return true;
	}

	return luaext_error_is_fatal(co, -1);
}

/*
 * Run one resume, leaving either the yielded/returned values or the error on
 * `co`. Shared by resume and wrap, which differ only in what they do with the
 * outcome.
 */
static int luaext_corolib_do_resume(lua_State *L, lua_State *co, int nargs, int *nres)
{
	luaext_sandbox *sandbox = LUAEXT_SB(L);
	lua_State *previous;
	uint32_t depth_cap;
	int status;

	depth_cap = sandbox->policy.limits.max_coroutine_depth;

	if (depth_cap != 0 && sandbox->co_depth >= depth_cap) {
		luaext_error_raise(L, LUAEXT_ERR_COROUTINE, true,
						   "Resuming here would nest %u coroutine(s) deep, which is the "
						   "sandbox's Limits::$maxCoroutineDepth",
						   (unsigned int)depth_cap + 1);
	}

	lua_xmove(L, co, nargs);

	/*
	 * running_L is what makes an interrupt land on the coroutine that is
	 * actually executing rather than on the main thread. Saved and restored
	 * rather than cleared, because a resume can nest.
	 */
	previous = sandbox->running_L;
	sandbox->running_L = co;
	sandbox->co_depth++;

	if (sandbox->co_depth > sandbox->co_peak_depth) {
		sandbox->co_peak_depth = sandbox->co_depth;
	}

	status = lua_resume(co, L, nargs, nres);

	sandbox->co_depth--;
	sandbox->running_L = previous;

	return status;
}

static int luaext_corolib_resume(lua_State *L)
{
	lua_State *co = lua_tothread(L, 1);
	int nargs;
	int nres;
	int status;

	luaL_argexpected(L, co != NULL, 1, "coroutine");

	nargs = lua_gettop(L) - 1;

	if (!lua_checkstack(co, nargs + 1)) {
		luaL_error(L, "too many arguments to resume");
	}

	status = luaext_corolib_do_resume(L, co, nargs, &nres);

	if (status == LUA_OK || status == LUA_YIELD) {
		if (!lua_checkstack(L, nres + 1)) {
			lua_pop(co, nres);
			luaL_error(L, "too many results to resume");
		}

		lua_pushboolean(L, 1);
		lua_xmove(co, L, nres);

		return nres + 1;
	}

	/*
	 * The line this wrapper exists for. Upstream returns `false, err` here for
	 * everything, which turns resume into a pcall that can swallow a tripped
	 * limit -- a script moves its infinite loop into a coroutine and catches its
	 * own CPU breach.
	 *
	 * Converted rather than merely re-raised for LUA_ERRMEM, for the reason
	 * luaext_baselib.c spells out: re-raising the plain string would leave the
	 * enclosing protected call seeing LUA_ERRRUN, and a nested pcall would catch
	 * what this one refused.
	 */
	if (luaext_corolib_is_fatal(co, status)) {
		if (status == LUA_ERRMEM) {
			lua_pop(co, 1);
			luaext_error_raise(L, LUAEXT_ERR_MEMORY, true,
							   "The sandbox is out of memory; a script may not catch its own "
							   "memory limit being reached");
		}

		lua_xmove(co, L, 1);

		return lua_error(L);
	}

	lua_pushboolean(L, 0);
	lua_xmove(co, L, 1);

	return 2;
}

/* -------------------------------------------------------------------------
 * wrap
 * ---------------------------------------------------------------------- */

static int luaext_corolib_wrapped(lua_State *L)
{
	lua_State *co = lua_tothread(L, lua_upvalueindex(1));
	int nargs = lua_gettop(L);
	int nres;
	int status;

	if (!lua_checkstack(co, nargs + 1)) {
		luaL_error(L, "too many arguments to resume");
	}

	status = luaext_corolib_do_resume(L, co, nargs, &nres);

	if (status == LUA_OK || status == LUA_YIELD) {
		if (!lua_checkstack(L, nres + 1)) {
			lua_pop(co, nres);
			luaL_error(L, "too many results to resume");
		}

		lua_xmove(co, L, nres);

		return nres;
	}

	/*
	 * wrap propagates every error, so it looks safe already -- until a pcall is
	 * put around it, which is the second attack in
	 * tests/03-adversarial/coroutine-cannot-swallow-fatal.phpt. A fatal has to
	 * arrive at that pcall as the unforgeable marker, or the enclosing pcall
	 * catches it like any other runtime error.
	 */
	if (status == LUA_ERRMEM) {
		lua_pop(co, 1);
		luaext_error_raise(L, LUAEXT_ERR_MEMORY, true,
						   "The sandbox is out of memory; a script may not catch its own memory "
						   "limit being reached");
	}

	lua_xmove(co, L, 1);

	return lua_error(L);
}

static int luaext_corolib_wrap(lua_State *L)
{
	/* create does the cap check and the tracking; doing it here too would
	 * count one coroutine twice. */
	luaext_corolib_create(L);
	lua_pushcclosure(L, luaext_corolib_wrapped, 1);

	return 1;
}

/* -------------------------------------------------------------------------
 * The rest, which upstream's semantics already satisfy
 * ---------------------------------------------------------------------- */

static int luaext_corolib_yield(lua_State *L)
{
	return lua_yield(L, lua_gettop(L));
}

static int luaext_corolib_status(lua_State *L)
{
	lua_State *co = lua_tothread(L, 1);
	const char *name;

	luaL_argexpected(L, co != NULL, 1, "coroutine");

	if (L == co) {
		name = "running";
	} else {
		switch (lua_status(co)) {
		case LUA_YIELD:
			name = "suspended";
			break;

		case LUA_OK:
			if (lua_getstack(co, 0, &(lua_Debug){0}) > 0) {
				name = "normal"; /* it resumed someone else */
			} else if (lua_gettop(co) == 0) {
				name = "dead";
			} else {
				name = "suspended"; /* created, never resumed */
			}
			break;

		default:
			name = "dead"; /* it finished with an error */
			break;
		}
	}

	lua_pushstring(L, name);

	return 1;
}

static int luaext_corolib_running(lua_State *L)
{
	int main_thread = lua_pushthread(L);

	lua_pushboolean(L, main_thread);

	return 2;
}

static int luaext_corolib_isyieldable(lua_State *L)
{
	lua_State *co = lua_isnoneornil(L, 1) ? L : lua_tothread(L, 1);

	luaL_argexpected(L, co != NULL, 1, "coroutine");
	lua_pushboolean(L, lua_isyieldable(co));

	return 1;
}

static int luaext_corolib_close(lua_State *L)
{
	lua_State *co = lua_tothread(L, 1);
	int status;

	luaL_argexpected(L, co != NULL, 1, "coroutine");

	status = lua_closethread(co, L);

	if (status == LUA_OK) {
		lua_pushboolean(L, 1);
		return 1;
	}

	/*
	 * A <close> handler that tripped a limit must not be reportable as a
	 * catchable `false, err` -- that would be the swallow this file prevents
	 * everywhere else, reached through a different door.
	 */
	if (luaext_corolib_is_fatal(co, status)) {
		lua_xmove(co, L, 1);
		return lua_error(L);
	}

	lua_pushboolean(L, 0);
	lua_xmove(co, L, 1);

	return 2;
}

/* -------------------------------------------------------------------------
 * Install and sweep
 * ---------------------------------------------------------------------- */

bool luaext_corolib_install(lua_State *L, luaext_sandbox *sandbox)
{
	if (!luaext_has_cap(&sandbox->policy, LUAEXT_CAP_COROUTINES)) {
		return true;
	}

	luaL_checkstack(L, 8, "luaext: no stack to build the coroutine library");

	lua_createtable(L, 0, 8);

	lua_pushcfunction(L, luaext_corolib_create);
	lua_setfield(L, -2, "create");

	lua_pushcfunction(L, luaext_corolib_resume);
	lua_setfield(L, -2, "resume");

	lua_pushcfunction(L, luaext_corolib_wrap);
	lua_setfield(L, -2, "wrap");

	lua_pushcfunction(L, luaext_corolib_yield);
	lua_setfield(L, -2, "yield");

	lua_pushcfunction(L, luaext_corolib_status);
	lua_setfield(L, -2, "status");

	lua_pushcfunction(L, luaext_corolib_running);
	lua_setfield(L, -2, "running");

	lua_pushcfunction(L, luaext_corolib_isyieldable);
	lua_setfield(L, -2, "isyieldable");

	lua_pushcfunction(L, luaext_corolib_close);
	lua_setfield(L, -2, "close");

	lua_setglobal(L, LUA_COLIBNAME);

	/* Pre-create the tracking table so the first create() does not have to, and
	 * so the sweep can assume it exists. */
	luaext_corolib_push_threads(L);
	lua_pop(L, 1);

	return true;
}

void luaext_corolib_sweep(luaext_sandbox *sandbox)
{
	lua_State *L;

	if (sandbox == NULL || sandbox->L == NULL || sandbox->co_live == 0) {
		return;
	}

	L = sandbox->L;

	if (!lua_checkstack(L, 4)) {
		return;
	}

	luaext_corolib_push_threads(L);

	lua_pushnil(L);

	while (lua_next(L, -2) != 0) {
		lua_State *co = lua_tothread(L, -2);

		lua_pop(L, 1); /* value; the key stays for lua_next */

		if (co != NULL && co != L) {
			/*
			 * Return value deliberately ignored. A <close> handler that raised
			 * has nowhere to report to -- this runs between the script finishing
			 * and the boundary returning, with no protected call in between --
			 * and the sticky interrupt flag, still set at this point, is what
			 * actually stops a handler that tripped a limit. Every thread gets
			 * closed either way; one misbehaving handler must not strand the
			 * rest.
			 */
			(void)lua_closethread(co, L);
		}
	}

	lua_pop(L, 1);

	/*
	 * Emptied rather than left to the collector: the guarantee is that no
	 * suspended state survives the call, and a dead thread still in the table
	 * would keep the count wrong for the next one.
	 */
	lua_pushnil(L);
	lua_rawsetp(L, LUA_REGISTRYINDEX, &luaext_key_threads);

	sandbox->co_live = 0;
	sandbox->co_depth = 0;
}
