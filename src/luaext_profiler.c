/*
 * luaext — the sampling profiler. See luaext_profiler.h for why it is opt-in.
 */

#include "luaext_profiler.h"

#include "luaext_timers.h"
#include "luaext_watchdog.h"

#include <lauxlib.h>
#include <lua.h>

#include <string.h>

/*
 * Nominal instructions per second, used only to turn the requested period into
 * a hook count.
 *
 * Deliberately a constant rather than something calibrated. Getting it wrong
 * changes how OFTEN samples are taken and nothing else: every reported figure is
 * that function's share of the total samples, scaled by CPU time that was
 * measured independently. A machine twice this fast simply samples twice as
 * often as asked and reports the same proportions.
 */
#define LUAEXT_PROFILER_NOMINAL_IPS 50000000.0

/* Distinct functions tracked before new ones are ignored.
 *
 * A bound is needed because the key is built from the chunk name, and a script
 * that loads generated chunks in a loop would otherwise grow this table for as
 * long as it ran -- turning a diagnostic into a memory leak the memory limit
 * does not see, since this lives outside the Lua heap. */
#define LUAEXT_PROFILER_MAX_FUNCTIONS 4096u

struct luaext_profiler {
	HashTable counts; /* identity -> sample count */
	uint64_t total;
	bool enabled;
	bool overflowed; /* a function was dropped at the cap */
};

bool luaext_profiler_active(const luaext_sandbox *sandbox)
{
	return sandbox->profiler != NULL && sandbox->profiler->enabled;
}

/*
 * Name the function that was running when the hook fired.
 *
 * "what" tells C functions from Lua ones, and a C function is reported by name
 * alone because it has no source position. A Lua function is reported as
 * chunk:line, which is stable across calls and is what a host needs to find it.
 * Neither form carries an address, for the reason the vendored tostring patch
 * exists: a heap address is a disclosure, and a profile is a thing hosts print.
 */
static void luaext_profiler_identify(lua_State *L, lua_Debug *ar, char *out, size_t out_size)
{
	if (ar->what != NULL && strcmp(ar->what, "C") == 0) {
		snprintf(out, out_size, "[C] %s", ar->name != NULL ? ar->name : "?");
		return;
	}

	if (ar->name != NULL) {
		snprintf(out, out_size, "%s:%d (%s)", ar->short_src, ar->linedefined, ar->name);
		return;
	}

	snprintf(out, out_size, "%s:%d", ar->short_src, ar->linedefined);

	(void)L;
}

static void luaext_profiler_hook(lua_State *L, lua_Debug *ar)
{
	luaext_sandbox *sandbox = LUAEXT_SB(L);
	char identity[256];
	size_t identity_len;
	zval *slot;

	if (sandbox == NULL || sandbox->profiler == NULL || !sandbox->profiler->enabled) {
		return;
	}

	/*
	 * The activation record the hook was handed already names the function that
	 * was running -- it only needs filling in. Walking to a level instead is the
	 * mistake this replaced: lua_getstack(L, 1) is the CALLER, so every sample
	 * landed on whatever invoked the hot function, and a whole profile collapsed
	 * onto the main chunk.
	 */
	if (lua_getinfo(L, "nS", ar) == 0) {
		return;
	}

	luaext_profiler_identify(L, ar, identity, sizeof(identity));

	sandbox->profiler->total++;
	identity_len = strlen(identity);

	/*
	 * The str_ variants throughout, and that is not a style choice. The counts
	 * table is persistent, so a key built with zend_string_init(.., 0) belongs to
	 * the request allocator and the table would later free it with the wrong one
	 * -- which crashed on the very first sample. zend_hash_str_add_new builds the
	 * key with the TABLE's persistence flag, so the two can never disagree.
	 *
	 * It is also the faster path: the lookup that hits, which is almost all of
	 * them, allocates nothing at all.
	 */
	slot = zend_hash_str_find(&sandbox->profiler->counts, identity, identity_len);

	if (slot != NULL) {
		Z_LVAL_P(slot)++;
	} else if (zend_hash_num_elements(&sandbox->profiler->counts) < LUAEXT_PROFILER_MAX_FUNCTIONS) {
		zval one;

		ZVAL_LONG(&one, 1);
		zend_hash_str_add_new(&sandbox->profiler->counts, identity, identity_len, &one);
	} else {
		/* Recorded rather than silently dropped: a profile missing its most
		 * expensive function because the table filled up would be actively
		 * misleading, so getProfile() reports the truncation. */
		sandbox->profiler->overflowed = true;
	}
}

bool luaext_profiler_enable(luaext_sandbox *sandbox, double period_seconds)
{
	int count;

	/*
	 * The refusal that matters. When the watchdog thread could not start, the
	 * count hook IS the CPU limit -- see luaext_timers.c. Replacing it with a
	 * profiling hook would leave the limit configured, reported as enforced, and
	 * doing nothing.
	 */
	if (luaext_watchdog_thread_failed() && LUAEXT_G(hook_count) > 0) {
		return false;
	}

	if (sandbox->profiler == NULL) {
		sandbox->profiler = pemalloc(sizeof(*sandbox->profiler), 1);
		memset(sandbox->profiler, 0, sizeof(*sandbox->profiler));
		zend_hash_init(&sandbox->profiler->counts, 64, NULL, NULL, 1);
	}

	count = (int)(period_seconds * LUAEXT_PROFILER_NOMINAL_IPS);

	if (count < 1) {
		count = 1;
	}

	sandbox->profiler->enabled = true;

	lua_sethook(sandbox->L, luaext_profiler_hook, LUA_MASKCOUNT, count);

	return true;
}

void luaext_profiler_disable(luaext_sandbox *sandbox)
{
	if (sandbox->profiler == NULL || !sandbox->profiler->enabled) {
		return;
	}

	sandbox->profiler->enabled = false;

	/*
	 * Cleared outright rather than restored to whatever was there. Nothing else
	 * arms a hook while profiling is possible: the timers hook only exists when
	 * the watchdog failed, and enable() refuses in exactly that case.
	 */
	lua_sethook(sandbox->L, NULL, 0, 0);
}

/*
 * Descending by cost, which is what "most expensive first" means.
 *
 * Written out rather than borrowed: ext/standard's array comparators are static
 * to that extension, and zend_array_sort takes a function POINTER -- passing
 * NULL for "the default" compiles cleanly and calls address zero the moment a
 * profile is read.
 */
static int luaext_profiler_compare(Bucket *left, Bucket *right)
{
	double a = Z_TYPE(left->val) == IS_DOUBLE ? Z_DVAL(left->val) : 0.0;
	double b = Z_TYPE(right->val) == IS_DOUBLE ? Z_DVAL(right->val) : 0.0;

	if (a == b) {
		return 0;
	}

	return a < b ? 1 : -1;
}

void luaext_profiler_result(luaext_sandbox *sandbox, uint8_t unit, zval *out)
{
	zend_string *name;
	zval *slot;
	double scale = 1.0;

	array_init(out);

	if (sandbox->profiler == NULL || sandbox->profiler->total == 0) {
		return;
	}

	/*
	 * Each function's SHARE of the samples, scaled by a quantity measured
	 * elsewhere. This is what makes the nominal instruction rate harmless: it
	 * decides how many samples were taken, never what fraction of the run each
	 * function accounts for.
	 */
	switch (unit) {
	case 1: /* Seconds */
		scale = luaext_timers_cpu_seconds(sandbox) / (double)sandbox->profiler->total;
		break;
	case 2: /* Percent */
		scale = 100.0 / (double)sandbox->profiler->total;
		break;
	default: /* Samples */
		scale = 1.0;
		break;
	}

	ZEND_HASH_FOREACH_STR_KEY_VAL(&sandbox->profiler->counts, name, slot)
	{
		if (name == NULL) {
			continue;
		}

		add_assoc_double_ex(out, ZSTR_VAL(name), ZSTR_LEN(name), (double)Z_LVAL_P(slot) * scale);
	}
	ZEND_HASH_FOREACH_END();

	/* Most expensive first, as the stub documents. */
	zend_hash_sort(Z_ARRVAL_P(out), luaext_profiler_compare, 0);

	if (sandbox->profiler->overflowed) {
		add_assoc_double(out,
						 "[truncated: more than " ZEND_TOSTR(
							 LUAEXT_PROFILER_MAX_FUNCTIONS) " functions sampled]",
						 0.0);
	}
}

void luaext_profiler_shutdown(luaext_sandbox *sandbox)
{
	if (sandbox == NULL || sandbox->profiler == NULL) {
		return;
	}

	zend_hash_destroy(&sandbox->profiler->counts);
	pefree(sandbox->profiler, 1);
	sandbox->profiler = NULL;
}
