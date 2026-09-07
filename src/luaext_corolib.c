/*
 * luaext — the coroutine library, wrapped. See luaext_corolib.h for why.
 */

#include "luaext_corolib.h"

#include "luaext_error.h"

#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>

#include <string.h>

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

static const char *luaext_corolib_status_name(lua_State *L, lua_State *co);

/*
 * Count the tracking table's entries, walking on whichever state is executing.
 *
 * only_alive selects between the two populations this file has to keep
 * straight. The cap and the stat ask about threads that can still run
 * (status != "dead"), so Limits::$maxLiveCoroutines and
 * SandboxStats::$liveCoroutines describe the same thing and the figure a host
 * reads can predict the refusal. The sweep's bookkeeping needs plain
 * membership: a dead-but-referenced thread may still hold unclosed <close>
 * variables that must run at end-of-call, so it has to keep the sweep's
 * early-out armed even though nothing counts it as alive.
 *
 * The table is asked for, never created: counting is an observer -- this runs
 * from stats() among other places -- and creating the table would grow the
 * very heap the caller just sampled. No table means nothing to count: the
 * capability is off, or the sweep detached it on its way out.
 */
static uint32_t luaext_corolib_count(lua_State *L, bool only_alive)
{
	uint32_t counted = 0;

	if (lua_rawgetp(L, LUA_REGISTRYINDEX, &luaext_key_threads) != LUA_TTABLE) {
		lua_pop(L, 1);
		return 0;
	}

	lua_pushnil(L);

	while (lua_next(L, -2) != 0) {
		lua_State *co = lua_tothread(L, -2);

		lua_pop(L, 1); /* value; the key stays for lua_next */

		/*
		 * The weak table only drops a dead thread at the next collection, so
		 * membership alone over-counts the living; status is what separates a
		 * thread that finished from one that is merely uncollected.
		 */
		if (co != NULL && (!only_alive || strcmp(luaext_corolib_status_name(L, co), "dead") != 0)) {
			counted++;
		}
	}

	lua_pop(L, 1);

	return counted;
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

	/*
	 * The sweep is closing this call's coroutines; a <close> handler creating
	 * new ones mid-sweep is exactly the suspended state the sweep exists to
	 * end, and inserting into the tracking table would rehash it under the
	 * sweep's own iterator. Catchable: the handler simply fails, and
	 * lua_closethread swallows what it raises.
	 */
	if (sandbox->co_sweeping) {
		return luaL_error(L,
						  "coroutines cannot be created while the call that owns them is closing");
	}

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
		/* A full collection is a full collection wherever it was decided:
		 * stats()->gcCollections counts this one like a script-issued
		 * collectgarbage("collect"). */
		sandbox->gc_collections++;

		/*
		 * co_live is refreshed from MEMBERSHIP: it is the sweep's early-out,
		 * and a dead thread the script still references must keep the sweep
		 * armed for its unclosed <close> variables. The refusal below judges
		 * the ALIVE population instead -- the one the limit's name promises
		 * and stats()->liveCoroutines reports -- so a host watching that
		 * figure can predict this refusal.
		 */
		sandbox->co_live = luaext_corolib_count(L, false);

		if (luaext_corolib_count(L, true) >= cap) {
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

/*
 * Close `co` with running_L pointing at it for the duration.
 *
 * lua_closethread runs the thread's <close> handlers ON `co`, and those are
 * script code like any resumed body: everything that resolves "the state that
 * is executing" from running_L -- interrupt delivery, the output layer's
 * exception reporting -- must see `co`, not whichever state asked for the
 * close. Raising on the wrong state is not cosmetic: the main thread has no
 * errorJmp here, so a raise routed to it would reach the panic handler and
 * take the request. Saved and restored rather than cleared, for the reason
 * do_resume records.
 *
 * The self-close (co == L from inside co) never returns and skips the restore,
 * which is harmless: running_L already names co on that path, and the resume
 * frame it unwinds to restores its own saved value.
 */
static int luaext_corolib_close_thread(luaext_sandbox *sandbox, lua_State *co, lua_State *from)
{
	lua_State *previous = sandbox->running_L;
	int status;

	sandbox->running_L = co;
	status = lua_closethread(co, from);
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

	/*
	 * MOVE FIRST, THEN INSERT THE false BENEATH IT. The obvious spelling --
	 * push false, then xmove the error across -- is wrong when `co` IS `L`,
	 * which happens the moment a coroutine resumes itself: lua_xmove returns
	 * immediately when its two states are the same, so the error stays where it
	 * already was and the false lands ON TOP of it. resume then answered
	 * `"cannot resume non-suspended coroutine", false` instead of
	 * `false, "cannot resume non-suspended coroutine"` -- the documented pair,
	 * backwards, on the one path a script reaches by accident.
	 *
	 * This spelling is correct either way: the xmove is a no-op exactly when the
	 * error is already on this stack, and lua_insert operates on one stack.
	 */
	lua_xmove(co, L, 1);
	lua_pushboolean(L, 0);
	lua_insert(L, -2);

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
	 * An error raised INSIDE the coroutine -- as opposed to a refused resume,
	 * which leaves lua_status(co) untouched -- leaves its to-be-closed
	 * variables pending. Upstream's luaB_auxwrap closes them right here, so a
	 * wrapped coroutine's <close> handlers run at the error rather than
	 * whenever the collector finds the thread, or never. A handler that
	 * itself raises replaces both the status and the error object, which is
	 * why the fatality decisions below run on what lua_closethread left, not
	 * on what lua_resume reported.
	 */
	if (lua_status(co) != LUA_OK && lua_status(co) != LUA_YIELD) {
		status = luaext_corolib_close_thread(LUAEXT_SB(L), co, L);
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

	/*
	 * Upstream prefixes a plain string error with the wrap CALLER's position,
	 * which is information only this frame has -- by the time the error
	 * reaches a handler, level 1 is somebody else. Only strings: the
	 * extension's own error userdata must arrive at the boundary intact, and
	 * LUA_ERRMEM was already converted above.
	 */
	if (lua_type(L, -1) == LUA_TSTRING) {
		luaL_where(L, 1);
		lua_insert(L, -2);
		lua_concat(L, 2);
	}

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

/*
 * The one place a coroutine's status is decided.
 *
 * Split out from the status() method because close() has to make the same
 * judgement: a normal coroutine cannot be closed, a running one only by
 * itself, and asking lua_closethread() to ignore either rule resets a stack
 * that a frame below is still executing on.
 */
static const char *luaext_corolib_status_name(lua_State *L, lua_State *co)
{
	if (L == co) {
		return "running";
	}

	switch (lua_status(co)) {
	case LUA_YIELD:
		return "suspended";

	case LUA_OK:
		if (lua_getstack(co, 0, &(lua_Debug){0}) > 0) {
			return "normal"; /* it resumed someone else */
		}

		return lua_gettop(co) == 0 ? "dead" : "suspended"; /* created, never resumed */

	default:
		return "dead"; /* it finished with an error */
	}
}

static int luaext_corolib_status(lua_State *L)
{
	lua_State *co = lua_tothread(L, 1);

	luaL_argexpected(L, co != NULL, 1, "coroutine");
	lua_pushstring(L, luaext_corolib_status_name(L, co));

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
	const char *state;
	int status;

	luaL_argexpected(L, co != NULL, 1, "coroutine");

	/*
	 * A NORMAL coroutine -- one that resumed somebody else and is waiting for
	 * them -- may not be closed, and the check has to be here rather than
	 * left to lua_closethread(), which does not make it: closing resets a
	 * stack that a frame below is still executing on.
	 */
	state = luaext_corolib_status_name(L, co);

	if (strcmp(state, "normal") == 0) {
		return luaL_error(L, "cannot close a %s coroutine", state);
	}

	/*
	 * A RUNNING coroutine here means co == L: the call is inside the very
	 * coroutine it names. Lua 5.5 defines that self-close -- lua_closethread
	 * runs the thread's <close> handlers and unwinds straight to the resume
	 * point, never returning here -- so it is allowed, exactly as upstream's
	 * luaB_close allows it. Only the main thread stays refused: it has no
	 * resume point to unwind to. (An earlier guard refused every running
	 * coroutine, reasoning from the 5.4-era lua_resetthread; 5.5's
	 * lua_closethread is specified for the self-close.)
	 */
	if (strcmp(state, "running") == 0) {
		lua_geti(L, LUA_REGISTRYINDEX, LUA_RIDX_MAINTHREAD);

		if (lua_tothread(L, -1) == co) {
			return luaL_error(L, "cannot close main thread");
		}

		luaext_corolib_close_thread(LUAEXT_SB(L), co, L);
		/* The self-close does not return. */
	}

	status = luaext_corolib_close_thread(LUAEXT_SB(L), co, L);

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

	/* Move then insert, for the reason resume spells out above. Unreachable
	 * with co == L now that a running coroutine is refused, but written the
	 * safe way regardless -- the two should not drift apart. */
	lua_xmove(co, L, 1);
	lua_pushboolean(L, 0);
	lua_insert(L, -2);

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

	/*
	 * Closing a thread runs its <close> handlers, and a handler creating a
	 * coroutine mid-sweep would insert into the table lua_next is walking: the
	 * rehash skips entries, and a skipped suspended coroutine survives the call
	 * that created it. Two defences, both needed -- creation is refused for the
	 * duration (see luaext_corolib_create), which is what makes co_live = 0
	 * below true rather than asserted, and the table is detached so a handler
	 * cannot reach the one being walked.
	 */
	sandbox->co_sweeping = true;

	if (lua_rawgetp(L, LUA_REGISTRYINDEX, &luaext_key_threads) == LUA_TTABLE) {
		lua_pushnil(L);
		lua_rawsetp(L, LUA_REGISTRYINDEX, &luaext_key_threads);

		lua_pushnil(L);

		while (lua_next(L, -2) != 0) {
			lua_State *co = lua_tothread(L, -2);

			lua_pop(L, 1); /* value; the key stays for lua_next */

			if (co != NULL && co != L) {
				/*
				 * Return value deliberately ignored. A <close> handler that
				 * raised has nowhere to report to -- this runs between the
				 * script finishing and the boundary returning, with no
				 * protected call in between -- and the sticky interrupt flag,
				 * still set at this point, is what actually stops a handler
				 * that tripped a limit. Every thread gets closed either way;
				 * one misbehaving handler must not strand the rest. A host
				 * exception a handler provoked survives regardless: the output
				 * layer declines to raise while co_sweeping is set, so it
				 * stays pending in PHP rather than being caught and dropped
				 * here.
				 */
				(void)luaext_corolib_close_thread(sandbox, co, L);
			}
		}
	}

	lua_pop(L, 1); /* the detached table, or whatever non-table was found */

	sandbox->co_sweeping = false;

	sandbox->co_live = 0;
	sandbox->co_depth = 0;
}

uint32_t luaext_corolib_live_count(const luaext_sandbox *sandbox)
{
	lua_State *L = sandbox->L;

	/*
	 * The fallbacks answer with the running counter: after close() there is
	 * no table left to ask (and the sweep zeroed the counter anyway), during
	 * the sweep the table is detached, and a stack that cannot grow cannot
	 * walk. Everywhere else the table is the truth and the counter is only a
	 * high-water mark -- it counts threads that finished and were collected
	 * until something recounts them away.
	 */
	if (L == NULL || sandbox->co_sweeping || !lua_checkstack(L, 3)) {
		return sandbox->co_live;
	}

	return luaext_corolib_count(L, true);
}

void luaext_corolib_set_hook_all(luaext_sandbox *sandbox, lua_Hook hook, int mask, int count)
{
	lua_State *L = sandbox->L;

	if (L == NULL) {
		return;
	}

	lua_sethook(L, hook, mask, count);

	/*
	 * A stack that cannot grow forfeits the walk, not the main hook above --
	 * the same give-up as the sweep. The callers treat a partially armed
	 * profile as a profile, which it still is: sampling is statistical, and
	 * this branch needs an allocator failure to be reached at all.
	 */
	if (!lua_checkstack(L, 3)) {
		return;
	}

	/* Same non-creating lookup as the recount, same reason: arming hooks is an
	 * observer, and with no table there is no thread to arm them on. */
	if (lua_rawgetp(L, LUA_REGISTRYINDEX, &luaext_key_threads) != LUA_TTABLE) {
		lua_pop(L, 1);
		return;
	}

	lua_pushnil(L);

	while (lua_next(L, -2) != 0) {
		lua_State *co = lua_tothread(L, -2);

		lua_pop(L, 1); /* value; the key stays for lua_next */

		if (co != NULL && co != L) {
			lua_sethook(co, hook, mask, count);
		}
	}

	lua_pop(L, 1);
}
