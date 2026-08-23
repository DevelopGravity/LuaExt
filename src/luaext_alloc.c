/*
 * luaext — memory accounting.
 *
 * Two counters make Limits::$memoryBytes a real ceiling rather than a number
 * in a struct: `usage` is every live byte Lua asked us for, and `charged` is
 * every live byte the host holds on the script's behalf (VFS file buffers, the
 * output buffer). The limit applies to their sum, because a script that can
 * spend host memory through the filesystem while staying inside its Lua budget
 * has not been limited at all.
 *
 * Everything here runs on the interpreter's hottest path, so this file calls
 * into neither PHP nor Lua's allocating API, never logs, and never allocates
 * anything of its own. malloc/realloc/free rather than emalloc: a sandbox may
 * legally outlive the request that built it in a worker SAPI, and request-local
 * memory would be freed underneath it.
 */

#include "luaext_alloc.h"

#include <stdlib.h>

/* -------------------------------------------------------------------------
 * Collector tuning
 *
 * Refusing an allocation is a last resort: Lua turns it into a memory error
 * that unwinds the script. A peak that a collection could have absorbed should
 * never get that far, so the collector is told to work harder the closer the
 * sandbox gets to its ceiling.
 *
 * Lua 5.5 replaced LUA_GCSETPAUSE/LUA_GCSETSTEPMUL with a single LUA_GCPARAM
 * option taking a parameter selector and a value:
 *
 *     lua_gc(L, LUA_GCPARAM, LUA_GCPPAUSE, 150);
 *
 * (lua.h:340-357, lapi.c:1235-1243). A negative value queries without setting;
 * the call returns the previous value. It touches only global_State::gcparams,
 * so it neither collects nor allocates and is safe to call from inside the
 * allocator itself.
 *
 * The parameters that matter here:
 *
 *   LUA_GCPPAUSE       percent of the last cycle's live set at which the next
 *                      incremental cycle starts (lgc.c:1122 setpause). 100
 *                      means "start again immediately".
 *   LUA_GCPSTEPMUL     work units performed per word allocated, i.e. collector
 *                      speed (lgc.c:1711-1712 incstep).
 *   LUA_GCPMINORMUL    percent of new bytes that triggers a minor collection in
 *                      generational mode (lgc.c:1418).
 *   LUA_GCPMINORMAJOR  percent of old bytes at which minor collections give way
 *                      to a major one (lgc.c:1324).
 *
 * Both modes are tuned because a sandbox holding the gcControl capability can
 * switch the collector to generational, at which point the incremental knobs
 * stop being consulted.
 *
 * LUA_GCPSTEPSIZE is deliberately left alone: it is granularity, not speed
 * (work per allocated word is set by LUA_GCPSTEPMUL alone), and its default is
 * expressed in terms of sizeof(Table), an internal type this file cannot see.
 * ---------------------------------------------------------------------- */

typedef struct {
	int pause;
	int step_mul;
	int minor_mul;
	int minor_major;
} luaext_gc_tuning;

/*
 * Tier 0 restores Lua 5.5.1's own defaults (LUAI_GCPAUSE 250, LUAI_GCMUL 200,
 * LUAI_GENMINORMUL 20, LUAI_MINORMAJOR 70 — lgc.h:170-198), so dropping back
 * below the first threshold really does undo the tuning rather than leaving the
 * collector permanently agitated.
 */
static const luaext_gc_tuning luaext_gc_tiers[] = {
	{250, 200, 20, 70}, /* below half the limit: upstream defaults */
	{200, 400, 15, 50}, /* half */
	{150, 700, 10, 35}, /* three quarters */
	{100, 1000, 5, 20}, /* seven eighths: collect continuously */
};

#define LUAEXT_GC_TIERS ((size_t)(sizeof(luaext_gc_tiers) / sizeof(luaext_gc_tiers[0])))

/*
 * Which tier `live` falls in. Thresholds are halves, quarters and eighths of
 * the limit so this costs shifts rather than divisions; the allocator consults
 * it on every allocation that grows the heap.
 *
 * An unlimited sandbox has no ceiling to approach and stays on the defaults.
 */
static zend_always_inline size_t luaext_alloc_tier(size_t live, size_t limit)
{
	if (limit == 0) {
		return 0;
	}

	if (live >= limit - (limit >> 3)) {
		return 3;
	}

	if (live >= limit - (limit >> 2)) {
		return 2;
	}

	if (live >= limit >> 1) {
		return 1;
	}

	return 0;
}

void luaext_alloc_tune_gc(luaext_sandbox *sandbox)
{
	luaext_alloc *alloc = &sandbox->alloc;
	const luaext_gc_tuning *tuning;
	lua_State *L = sandbox->L;
	size_t tier;

	/*
	 * NULL while lua_newstate is still building the state — the allocator runs
	 * long before there is a state to tune — and again from the moment close()
	 * starts tearing one down.
	 */
	if (L == NULL) {
		return;
	}

	tier = luaext_alloc_tier(alloc->usage + alloc->charged, alloc->limit);

	/*
	 * gc_last_tune holds the tier currently installed, so the common case is a
	 * single comparison and no calls at all. Zero-initialised, which matches the
	 * defaults lua_newstate installs.
	 */
	if (tier == alloc->gc_last_tune) {
		return;
	}

	alloc->gc_last_tune = tier;
	tuning = &luaext_gc_tiers[tier];

	lua_gc(L, LUA_GCPARAM, LUA_GCPPAUSE, tuning->pause);
	lua_gc(L, LUA_GCPARAM, LUA_GCPSTEPMUL, tuning->step_mul);
	lua_gc(L, LUA_GCPARAM, LUA_GCPMINORMUL, tuning->minor_mul);
	lua_gc(L, LUA_GCPARAM, LUA_GCPMINORMAJOR, tuning->minor_major);
}

/* -------------------------------------------------------------------------
 * The allocator
 * ---------------------------------------------------------------------- */

/*
 * Whether `growth` more bytes fit under the ceiling.
 *
 * Written so nothing can wrap. `live` is the sum of two counters that only ever
 * track memory the process really holds, so it cannot itself overflow; the
 * script chooses `growth`, so that is where the care goes:
 *
 *   - with a limit, `live >= limit` is tested before `limit - live`, because
 *     lowering the limit at runtime legitimately leaves usage above it and the
 *     subtraction would otherwise wrap to a number nothing could exceed;
 *   - without one, the only remaining ceiling is the address space, so the
 *     charge is refused rather than allowed to wrap the counters and hand a
 *     later allocation a budget it has not got.
 */
static zend_always_inline bool luaext_alloc_fits(size_t live, size_t limit, size_t growth)
{
	if (limit == 0) {
		return growth <= SIZE_MAX - live;
	}

	return live < limit && growth <= limit - live;
}

void *luaext_lua_alloc(void *ud, void *ptr, size_t osize, size_t nsize)
{
	luaext_sandbox *sandbox = (luaext_sandbox *)ud;
	luaext_alloc *alloc = &sandbox->alloc;
	void *block;

	/*
	 * lua_Alloc passes a type tag rather than a size in `osize` when `ptr` is
	 * NULL (lmem.c:35-36 and luaM_malloc_ at lmem.c:206), so the old size is
	 * only meaningful for a block that actually exists. Reading the tag as a
	 * size would credit the sandbox thousands of bytes it never allocated.
	 */
	const size_t old = (ptr != NULL) ? osize : 0;

	if (nsize == 0) {
		free(ptr);

		/*
		 * Cannot underflow: `usage` is the sum of the sizes of exactly the live
		 * blocks, and Lua passes the size it was given (lmem.c:152 asserts the
		 * pairing). Left as a bare subtraction because every free in the
		 * interpreter goes through here.
		 */
		alloc->usage -= old;

		return NULL;
	}

	if (nsize > old) {
		const size_t growth = nsize - old;

		if (!luaext_alloc_fits(alloc->usage + alloc->charged, alloc->limit, growth)) {
			return NULL;
		}

		block = realloc(ptr, nsize);

		/*
		 * Nothing has been touched yet, so a failed allocation leaves every
		 * counter exactly as it was. Lua responds by running an emergency
		 * collection and calling back once more (lmem.c:162-170); if that second
		 * attempt is refused too it raises a memory error.
		 */
		if (block == NULL) {
			return NULL;
		}

		alloc->usage += growth;

		if (alloc->usage + alloc->charged > alloc->peak) {
			alloc->peak = alloc->usage + alloc->charged;
		}

		luaext_alloc_tune_gc(sandbox);

		return block;
	}

	/*
	 * Shrinking, or reallocating to the same size. `ptr` is necessarily
	 * non-NULL: a NULL pointer makes `old` zero, and nsize is not zero here, so
	 * the growth branch above would have taken it.
	 *
	 * A shrink is never refused. Lua asserts that a reallocation to a non-zero
	 * size succeeds (lmem.c:186) and luaM_saferealloc_ turns a NULL into a
	 * memory error, so refusing to shrink would strand a sandbox that has no
	 * way left to give memory back — precisely when it most needs one.
	 */
	block = realloc(ptr, nsize);

	if (block == NULL) {
		/*
		 * ISO C permits realloc to fail even when shrinking. Keeping the
		 * original, larger block is correct: the caller gets memory that is at
		 * least as big as it asked for, and free() does not care that it is
		 * bigger. Accounting follows what Lua believes it holds, which is also
		 * the size it will hand back on the eventual free.
		 */
		block = ptr;
	}

	alloc->usage -= old - nsize;

	return block;
}

/* -------------------------------------------------------------------------
 * Host-side charges
 * ---------------------------------------------------------------------- */

bool luaext_alloc_charge(luaext_sandbox *sandbox, size_t bytes)
{
	luaext_alloc *alloc = &sandbox->alloc;
	size_t live;

	if (bytes == 0) {
		return true;
	}

	live = alloc->usage + alloc->charged;

	if (!luaext_alloc_fits(live, alloc->limit, bytes)) {
		return false;
	}

	alloc->charged += bytes;
	live += bytes;

	if (live > alloc->peak) {
		alloc->peak = live;
	}

	luaext_alloc_tune_gc(sandbox);

	return true;
}

void luaext_alloc_discharge(luaext_sandbox *sandbox, size_t bytes)
{
	luaext_alloc *alloc = &sandbox->alloc;

	/*
	 * The contract is that a caller never discharges more than it charged, but
	 * this counter is written by several subsystems and an underflow here would
	 * not merely mis-report: a `charged` near SIZE_MAX wraps usage + charged
	 * round to a small number and the ceiling stops holding. Clamping keeps a
	 * miscount a miscount.
	 */
	if (bytes > alloc->charged) {
		bytes = alloc->charged;
	}

	alloc->charged -= bytes;

	luaext_alloc_tune_gc(sandbox);
}

/* -------------------------------------------------------------------------
 * Accessors
 * ---------------------------------------------------------------------- */

size_t luaext_alloc_usage(const luaext_sandbox *sandbox)
{
	return sandbox->alloc.usage + sandbox->alloc.charged;
}

size_t luaext_alloc_peak(const luaext_sandbox *sandbox)
{
	return sandbox->alloc.peak;
}

void luaext_alloc_set_limit(luaext_sandbox *sandbox, size_t limit)
{
	sandbox->alloc.limit = limit;

	/*
	 * Deliberately does not compare the new limit against current usage.
	 * Failing retroactively would mean unwinding allocations that have already
	 * succeeded and whose memory is already in use; the next request that would
	 * grow the heap is refused instead, which is the only point at which
	 * refusing is safe. Re-tuning immediately means the collector starts working
	 * towards the new ceiling straight away rather than at the next allocation.
	 */
	luaext_alloc_tune_gc(sandbox);
}
