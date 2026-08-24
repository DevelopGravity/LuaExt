/*
 * luaext — what the vendored patches cost, measured against stock Lua.
 *
 * The point of this harness is that it builds the SAME source twice.
 * third_party/lua-5.5.1/src/ is upstream 5.5.1 plus nine patches, every one of
 * them guarded by LUAEXT_LUA_HOOKS -- so compiling it with the macro at 0
 * produces stock Lua byte-for-byte, and compiling it at 1 produces ours. Same
 * compiler, same flags, same machine, same run. The difference is the patches
 * and nothing else.
 *
 * That matters because the obvious alternative -- timing a system `lua` binary
 * against this one -- measures the distribution's compiler flags as much as it
 * measures us, and would make any number here unquotable.
 *
 * What is NOT measured: the PHP boundary, conversion, the callback bridge, or
 * the watchdog thread. This is the interpreter alone, which is the only part a
 * Lua author's own benchmark would be comparable to.
 *
 * Build and run both halves with tools/bench-vm.sh.
 */

#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/*
 * The patched interpreter calls these. In the extension they belong to the
 * timer subsystem; here the flag is never raised, so the raise path is dead
 * code and the hook is never ours.
 *
 * Deliberately NOT stubbed out of the patches with another #if: the point is to
 * measure the code that actually ships, including the branch that tests the
 * flag, so what is timed is the real cost of the check rather than a version of
 * it that the compiler was told to ignore.
 */
#if LUAEXT_LUA_HOOKS
void luaext_raise_interrupt(lua_State *L)
{
	(void)luaL_error(L, "luaext: execution interrupted");
}

int luaext_hook_is_ours(lua_State *L)
{
	(void)L;
	return 0;
}
#endif

typedef struct {
	const char *name;
	const char *chunk;
} benchmark;

/*
 * Chosen so that each one is dominated by a different back edge, because that
 * is where the patches put their cost:
 *
 *   arithmetic / while   backward jump
 *   numeric for          the numeric-'for' back edge
 *   generic for          the generic-'for' back edge
 *   tail recursion       the tail-call check
 *   calls                ordinary calls, which are NOT patched -- a control
 *   table / string       C-heavy work, mostly outside the VM loop
 */
static const benchmark benchmarks[] = {
	{"arithmetic (while)", "local n = 0 local i = 0\n"
						   "while i < 20000000 do n = n + i i = i + 1 end return n"},
	{"numeric for", "local n = 0 for i = 1, 20000000 do n = n + i end return n"},
	{"generic for", "local t = {} for i = 1, 200000 do t[i] = i end\n"
					"local n = 0 for _ = 1, 50 do for _, v in ipairs(t) do n = n + v end end return n"},
	{"tail recursion", "local function f(n, acc) if n == 0 then return acc end\n"
					   "return f(n - 1, acc + n) end\n"
					   "local n = 0 for _ = 1, 8 do n = f(1000000, 0) end return n"},
	{"function calls", "local function add(a, b) return a + b end\n"
					   "local n = 0 for i = 1, 8000000 do n = add(n, i) end return n"},
	{"table writes", "local t = {} for r = 1, 40 do for i = 1, 200000 do t[i] = i * r end end\n"
					 "return #t"},
	{"string concat", "local n = 0 for i = 1, 400000 do n = n + #(\"ab\"):rep(4) end return n"},
	{"pcall churn", "local function ok() return 1 end\n"
					"local n = 0 for i = 1, 2000000 do local _, v = pcall(ok) n = n + v end return n"},
};

/*
 * The libraries a sandbox actually gets, opened one at a time.
 *
 * luaL_openlibs() is unavailable on purpose: it is a macro over
 * luaL_openselectedlibs(), which lives in linit.c, and SOURCES excludes that
 * file so nothing can open a library the policy did not choose. Listing them
 * here keeps the benchmark honest in the same way -- it measures an
 * interpreter with the surface the extension really exposes.
 */
#if LUAEXT_BENCH_HOOK

/*
 * The mechanism this design REPLACED, kept so the cost of it stays quotable.
 *
 * A LUA_MASKCOUNT hook whose body does what ours did: read the interrupt flag
 * and return. The body is almost free -- and that is the point. The expense is
 * not here at all. Any non-zero 'hookmask' forces 'ci->u.l.trap', and a set
 * trap makes 'vmfetch' call 'luaG_traceexec' on EVERY instruction, which is
 * why raising the count barely helps: the count throttles this function, not
 * the per-instruction call that reaches it.
 */
static void bench_count_hook(lua_State *L, lua_Debug *ar)
{
	(void)L;
	(void)ar;
}

#define LUAEXT_BENCH_HOOK_COUNT 1000

#endif

static void open_libraries(lua_State *L)
{
	static const luaL_Reg libraries[] = {
		{LUA_GNAME, luaopen_base},
		{LUA_TABLIBNAME, luaopen_table},
		{LUA_STRLIBNAME, luaopen_string},
		{LUA_MATHLIBNAME, luaopen_math},
		{NULL, NULL},
	};
	const luaL_Reg *library;

	for (library = libraries; library->name != NULL; library++) {
		luaL_requiref(L, library->name, library->func, 1);
		lua_pop(L, 1);
	}
}

static double now_seconds(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
		return 0.0;
	}

	return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* Best of N. The minimum is the least noisy estimator here: scheduler
 * interference and cache eviction can only ever make a run slower. */
static double run_best(lua_State *L, const char *chunk, int runs)
{
	double best = -1.0;
	int attempt;

	for (attempt = 0; attempt < runs; attempt++) {
		double start;
		double elapsed;

		if (luaL_loadstring(L, chunk) != LUA_OK) {
			fprintf(stderr, "load failed: %s\n", lua_tostring(L, -1));
			exit(1);
		}

		start = now_seconds();

		if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
			fprintf(stderr, "run failed: %s\n", lua_tostring(L, -1));
			exit(1);
		}

		elapsed = now_seconds() - start;

		if (best < 0.0 || elapsed < best) {
			best = elapsed;
		}

		lua_gc(L, LUA_GCCOLLECT);
	}

	return best;
}

int main(int argc, char **argv)
{
	int runs = (argc > 1) ? atoi(argv[1]) : 5;
	size_t index;

	if (runs < 1) {
		runs = 1;
	}

	for (index = 0; index < sizeof(benchmarks) / sizeof(benchmarks[0]); index++) {
		lua_State *L = luaL_newstate();
		double seconds;

		if (L == NULL) {
			fprintf(stderr, "no interpreter\n");
			return 1;
		}

		open_libraries(L);

#if LUAEXT_BENCH_HOOK
		lua_sethook(L, bench_count_hook, LUA_MASKCOUNT, LUAEXT_BENCH_HOOK_COUNT);
#endif

		seconds = run_best(L, benchmarks[index].chunk, runs);
		lua_close(L);

		/* Machine-readable: the shell wrapper joins the two runs on this name. */
		printf("%s\t%.6f\n", benchmarks[index].name, seconds);
	}

	return 0;
}
