/*
 * luaext — the os library a sandbox sees.
 *
 * This library is BUILT, not filtered. third_party/lua-5.5.1/SOURCES leaves
 * loslib.c out of the link, so system(), popen() and tmpnam() are absent from
 * the binary rather than merely unreachable from Lua, and there is no upstream
 * opener to select from. Everything published here is written or ported below.
 *
 * Scope for this wave is time and environment only:
 *
 *   clock                       always
 *   time, date, difftime        osTime   (on by default)
 *   getenv                      osEnv    (off by default, allow-list bound)
 *
 * The table always exists, with at least os.clock, so a script can call it
 * without a nil check. os.remove and os.rename belong to the VFS and arrive
 * with it; os.execute, os.exit, os.tmpname and os.setlocale never arrive.
 *
 * -------------------------------------------------------------------------
 * Derivation and licence
 *
 * os_date, os_time, os_difftime and their helpers (checkoption, getfield,
 * getboolfield, setfield, setboolfield, setallfields, l_checktime) are ported
 * from third_party/lua-5.5.1/src/loslib.c and are used under the vendored MIT
 * licence in third_party/lua-5.5.1/LICENSE. Reimplementing strftime marshalling
 * from scratch would be strictly worse: checkoption's LUA_STRFTIMEOPTIONS
 * allow-list is the security-relevant part, and it carries over verbatim so
 * that a Lua upgrade's changes to it are a visible diff rather than a silent
 * divergence.
 *
 * Four deliberate changes from upstream:
 *
 *   1. php_localtime_r / php_gmtime_r instead of upstream's l_localtime /
 *      l_gmtime. Upstream's macros resolve to the static-buffer ISO C
 *      localtime()/gmtime() unless LUA_USE_POSIX is defined, and that shared
 *      buffer is a ZTS data race between two requests formatting a date at the
 *      same time.
 *
 *   2. LUAEXT_CHECK() once per format specifier. A megabyte of "%c" is a cheap
 *      amplifier -- each two input bytes can become up to SIZETIMEFMT output
 *      bytes -- and strftime is a C loop no interrupt hook can reach into.
 *
 *   3. The result is refused when it would exceed Limits::$maxStringLength.
 *
 *   4. ct_diff2sz() is a plain (size_t) cast here; llimits.h is an internal
 *      Lua header and is not ours to include.
 *
 * -------------------------------------------------------------------------
 * Accepted disclosure
 *
 * os.date without a leading '!' formats in the process's local time zone, so a
 * script can infer the host's TZ (and, with os.time on a table, its DST rules).
 * That is accepted at the untrusted baseline: it is a property of the machine,
 * not of any other tenant, and withholding local time would make os.date
 * useless for the thing hosts actually want it for. A host that considers the
 * time zone sensitive should withhold osTime entirely.
 */

#include "luaext_openlibs.h"
#include "luaext_timers.h"
#include "luaext_vfs.h"

#include <lauxlib.h>
#include <lualib.h>

#include <main/php_reentrancy.h>

#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* -------------------------------------------------------------------------
 * os.clock
 * ---------------------------------------------------------------------- */

/*
 * The grid os.clock quantises to, in seconds (~20 us).
 *
 * Be honest about what this buys: it raises the cost of using os.clock as a
 * timing oracle by forcing an attacker to average over many samples. It does
 * not remove the oracle, and no rounding would. The real defence is what is
 * being measured -- this sandbox's OWN billed CPU, the same quantity its CPU
 * limit enforces, and nothing else in the process a script can name.
 */
#define LUAEXT_OSLIB_CLOCK_GRID 0.00002

/*
 * Above this the quantiser stops trying and answers with what it last reported.
 *
 * Nothing legitimate reaches it -- it is nine and a half years of CPU -- and the
 * point is that truncating an out-of-range double to an integer is undefined
 * behaviour, so a clock that ever returned an infinity or a NaN would be a
 * crash rather than a wrong number.
 */
#define LUAEXT_OSLIB_CLOCK_CEILING 300000000.0

/*
 * Per-sandbox clock state, held as os.clock's only upvalue.
 *
 * `last_reported` makes os.clock monotonic by construction rather than by
 * inheritance. The billed-CPU counter behind it is already monotonic, but a
 * script must never see time run backwards even if a future source is not, and
 * the quantiser's out-of-range branch needs something to answer with.
 */
typedef struct {
	double last_reported;
} luaext_oslib_clock_state;

static int luaext_oslib_clock(lua_State *L)
{
	luaext_oslib_clock_state *state =
		(luaext_oslib_clock_state *)lua_touserdata(L, lua_upvalueindex(1));
	luaext_sandbox *sandbox = LUAEXT_SB(L);
	double seconds = (sandbox != NULL) ? luaext_timers_cpu_seconds(sandbox) : 0.0;

	/*
	 * The timer layer is the only source now: it reports this sandbox's own
	 * billed CPU, which is the same quantity its CPU limit enforces, so a script
	 * timing itself measures exactly the budget it is spending.
	 *
	 * Zero is a legitimate answer -- a sandbox that has not run yet, or a
	 * platform with no per-thread CPU clock, where Sandbox::features() reports
	 * the limit as Unsupported rather than pretending. The monotonic latch below
	 * then holds it at whatever was last reported.
	 */

	/*
	 * Truncation towards zero rather than floor(): `seconds` cannot be negative
	 * here, so the two agree, and this keeps libm out of the link for a single
	 * rounding step. The NaN case falls out of the comparison, which is false
	 * for any NaN.
	 */
	if (seconds > 0.0 && seconds < LUAEXT_OSLIB_CLOCK_CEILING) {
		seconds = (double)(uint64_t)(seconds / LUAEXT_OSLIB_CLOCK_GRID) * LUAEXT_OSLIB_CLOCK_GRID;
	} else {
		seconds = state->last_reported;
	}

	if (seconds < state->last_reported) {
		seconds = state->last_reported;
	}

	state->last_reported = seconds;

	lua_pushnumber(L, (lua_Number)seconds);

	return 1;
}

/* -------------------------------------------------------------------------
 * os.getenv
 * ---------------------------------------------------------------------- */

/*
 * Longest environment value os.getenv will return.
 *
 * A fixed buffer rather than an allocation because the copy happens while the
 * TSRM environment lock is held: malloc and lua_pushlstring can both fail, and
 * failing under the lock would either longjmp out still holding it or leave a
 * buffer nothing frees. Nothing recursive can reach this function, so exactly
 * one of these frames exists at a time.
 */
#define LUAEXT_OSLIB_ENV_MAX 16384

/* Names that cannot denote an environment variable, and must never be looked
 * up: a NUL truncates the C string handed to getenv(), so an allow-list entry
 * and the name actually resolved could differ. Checked before the allow-list
 * lookup, so the refusal reveals nothing about the allow list's contents. */
static bool luaext_oslib_env_name_is_sane(const char *name, size_t length)
{
	return memchr(name, '\0', length) == NULL && memchr(name, '=', length) == NULL;
}

/* upvalue 1: the allow-list set, name -> true, materialised at install time. */
static int luaext_oslib_getenv(lua_State *L)
{
	char buffer[LUAEXT_OSLIB_ENV_MAX];
	size_t length = 0;
	const char *name;
	size_t name_length;
	bool present = false;
	bool too_long = false;

	name = luaL_checklstring(L, 1, &name_length);

	if (!luaext_oslib_env_name_is_sane(name, name_length)) {
		return luaL_argerror(L, 1, "an environment variable name cannot contain '\\0' or '='");
	}

	lua_settop(L, 1);
	lua_pushvalue(L, 1);

	/*
	 * Not allow-listed answers nil -- exactly what a variable that is not set
	 * answers. A script cannot tell "you may not read this" from "there is
	 * nothing to read", so the allow list itself does not leak.
	 */
	if (lua_rawget(L, lua_upvalueindex(1)) != LUA_TBOOLEAN) {
		lua_pushnil(L);
		return 1;
	}

	tsrm_env_lock();
	{
		const char *value = getenv(name);

		if (value != NULL) {
			present = true;
			length = strlen(value);

			if (length >= sizeof(buffer)) {
				too_long = true;
				length = 0;
			} else {
				memcpy(buffer, value, length);
			}
		}
	}
	tsrm_env_unlock();

	if (too_long) {
		return luaL_error(L, "os.getenv: the value of '%s' exceeds %I bytes", name,
						  (lua_Integer)(LUAEXT_OSLIB_ENV_MAX - 1));
	}

	if (!present) {
		lua_pushnil(L);
	} else {
		lua_pushlstring(L, buffer, length);
	}

	return 1;
}

/*
 * Push Capabilities::$osEnvAllowList as a set, once, at install time.
 *
 * Reading the PHP property here rather than per call is the point: os.getenv
 * then touches no zval at all, so it cannot be made to run PHP code, cannot
 * observe a host object that changed since construction, and cannot fail for a
 * reason that has nothing to do with the environment.
 */
static void luaext_oslib_push_env_allow_list(lua_State *L, luaext_sandbox *sandbox)
{
	zval *capabilities;
	zval *allow_list;
	zval *entry;
	zval capabilities_rv;
	zval allow_list_rv;

	lua_createtable(L, 0, 8);

	if (Z_TYPE(sandbox->config_zv) != IS_OBJECT) {
		return;
	}

	capabilities =
		zend_read_property(luaext_ce_sandbox_config, Z_OBJ(sandbox->config_zv), "capabilities",
						   sizeof("capabilities") - 1, 1, &capabilities_rv);

	if (capabilities == NULL || Z_TYPE_P(capabilities) != IS_OBJECT) {
		return;
	}

	allow_list = zend_read_property(luaext_ce_capabilities, Z_OBJ_P(capabilities), "osEnvAllowList",
									sizeof("osEnvAllowList") - 1, 1, &allow_list_rv);

	if (allow_list == NULL || Z_TYPE_P(allow_list) != IS_ARRAY) {
		return;
	}

	/*
	 * A push can raise on memory exhaustion, which longjmps out of the loop.
	 * That is safe here only because nothing in this function owns a reference:
	 * zend_read_property hands back the property slot without addref'ing it.
	 */
	ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(allow_list), entry)
	{
		zend_string *name;

		if (Z_TYPE_P(entry) != IS_STRING) {
			continue;
		}

		name = Z_STR_P(entry);

		/* Refused at the source too, so an unusable entry can never sit in the
		 * set looking like it grants something. */
		if (!luaext_oslib_env_name_is_sane(ZSTR_VAL(name), ZSTR_LEN(name))) {
			continue;
		}

		lua_pushlstring(L, ZSTR_VAL(name), ZSTR_LEN(name));
		lua_pushboolean(L, 1);
		lua_rawset(L, -3);
	}
	ZEND_HASH_FOREACH_END();
}

/* -------------------------------------------------------------------------
 * os.date, os.time, os.difftime
 *
 * Ported from third_party/lua-5.5.1/src/loslib.c -- see the file header.
 * ---------------------------------------------------------------------- */

/*
 * The strftime specifiers a script may use, verbatim from loslib.c. This is the
 * security-relevant half of the port: strftime's behaviour on a specifier the
 * platform does not implement is undefined, and this allow-list is what keeps a
 * script from reaching one. Options are grouped by length; a group of length 2
 * starts with "||".
 */
#if !defined(LUA_STRFTIMEOPTIONS) /* { */

#if defined(LUA_USE_WINDOWS)
#define LUA_STRFTIMEOPTIONS                                                                        \
	"aAbBcdHIjmMpSUwWxXyYzZ%"                                                                      \
	"||"                                                                                           \
	"#c#x#d#H#I#j#m#M#S#U#w#W#y#Y" /* two-char options */
#elif defined(LUA_USE_C89)		   /* C89 (only 1-char options) */
#define LUA_STRFTIMEOPTIONS "aAbBcdHIjmMpSUwWxXyYZ%"
#else /* C99 specification */
#define LUA_STRFTIMEOPTIONS                                                                        \
	"aAbBcCdDeFgGhHIjmMnprRStTuUVwWxXyYzZ%"                                                        \
	"||"                                                                                           \
	"EcECExEXEyEY"                                                                                 \
	"OdOeOHOIOmOMOSOuOUOVOwOWOy"
#endif

#endif /* } */

/* Maximum size for an individual strftime item, from loslib.c. */
#define SIZETIMEFMT 250

/* loslib.c represents time_t in Lua as a lua_Integer (LUA_NUMTIME unset). */
#define l_timet lua_Integer
#define l_pushtime(L, t) lua_pushinteger(L, (lua_Integer)(t))
#define l_gettime(L, arg) luaL_checkinteger(L, arg)

static void luaext_oslib_setfield(lua_State *L, const char *key, int value, int delta)
{
	lua_pushinteger(L, (lua_Integer)value + delta);
	lua_setfield(L, -2, key);
}

static void luaext_oslib_setboolfield(lua_State *L, const char *key, int value)
{
	if (value < 0) { /* undefined? */
		return;		 /* does not set field */
	}

	lua_pushboolean(L, value);
	lua_setfield(L, -2, key);
}

/* Set all fields from structure 'tm' in the table on top of the stack. */
static void luaext_oslib_setallfields(lua_State *L, struct tm *stm)
{
	luaext_oslib_setfield(L, "year", stm->tm_year, 1900);
	luaext_oslib_setfield(L, "month", stm->tm_mon, 1);
	luaext_oslib_setfield(L, "day", stm->tm_mday, 0);
	luaext_oslib_setfield(L, "hour", stm->tm_hour, 0);
	luaext_oslib_setfield(L, "min", stm->tm_min, 0);
	luaext_oslib_setfield(L, "sec", stm->tm_sec, 0);
	luaext_oslib_setfield(L, "yday", stm->tm_yday, 1);
	luaext_oslib_setfield(L, "wday", stm->tm_wday, 1);
	luaext_oslib_setboolfield(L, "isdst", stm->tm_isdst);
}

static int luaext_oslib_getboolfield(lua_State *L, const char *key)
{
	int res;

	res = (lua_getfield(L, -1, key) == LUA_TNIL) ? -1 : lua_toboolean(L, -1);
	lua_pop(L, 1);

	return res;
}

static int luaext_oslib_getfield(lua_State *L, const char *key, int d, int delta)
{
	int isnum;
	int t = lua_getfield(L, -1, key); /* get field and its type */
	lua_Integer res = lua_tointegerx(L, -1, &isnum);

	if (!isnum) {			 /* field is not an integer? */
		if (t != LUA_TNIL) { /* some other value? */
			return luaL_error(L, "field '%s' is not an integer", key);
		} else if (d < 0) { /* absent field; no default? */
			return luaL_error(L, "field '%s' missing in date table", key);
		}

		res = d;
	} else {
		if (!(res >= 0 ? res - delta <= INT_MAX : INT_MIN + delta <= res)) {
			return luaL_error(L, "field '%s' is out-of-bound", key);
		}

		res -= delta;
	}

	lua_pop(L, 1);

	return (int)res;
}

/*
 * Verbatim from loslib.c apart from the ct_diff2sz() call site, which is in the
 * caller. Changing this is changing what a script may hand to strftime.
 */
static const char *luaext_oslib_checkoption(lua_State *L, const char *conv, size_t convlen,
											char *buff)
{
	const char *option = LUA_STRFTIMEOPTIONS;
	unsigned oplen = 1; /* length of options being checked */

	for (; *option != '\0' && oplen <= convlen; option += oplen) {
		if (*option == '|') { /* next block? */
			oplen++;		  /* will check options with next length (+1) */
		} else if (memcmp(conv, option, oplen) == 0) { /* match? */
			memcpy(buff, conv, oplen);				   /* copy valid option to buffer */
			buff[oplen] = '\0';
			return conv + oplen; /* return next item */
		}
	}

	luaL_argerror(L, 1, lua_pushfstring(L, "invalid conversion specifier '%%%s'", conv));

	return conv; /* to avoid warnings */
}

static time_t luaext_oslib_checktime(lua_State *L, int arg)
{
	l_timet t = l_gettime(L, arg);

	luaL_argcheck(L, (time_t)t == t, arg, "time out-of-bounds");

	return (time_t)t;
}

/*
 * Refuse a result the sandbox's own string limit would not allow.
 *
 * Checked while the answer is still a luaL_Buffer, so the oversized string is
 * never materialised as a Lua value.
 */
static void luaext_oslib_check_result_length(lua_State *L, size_t length)
{
	luaext_sandbox *sandbox = LUAEXT_SB(L);
	size_t limit = (sandbox != NULL) ? sandbox->policy.limits.max_string_length : 0;

	if (limit != 0 && length > limit) {
		luaL_error(L,
				   "os.date: the result would exceed the sandbox's maximum string length (%I "
				   "bytes)",
				   (lua_Integer)limit);
	}
}

static int luaext_oslib_date(lua_State *L)
{
	size_t slen;
	const char *s = luaL_optlstring(L, 1, "%c", &slen);
	time_t t = luaL_opt(L, luaext_oslib_checktime, 2, time(NULL));
	const char *se = s + slen; /* 's' end */
	struct tm tmr, *stm;

	if (*s == '!') { /* UTC? */
		stm = php_gmtime_r(&t, &tmr);
		s++; /* skip '!' */
	} else {
		stm = php_localtime_r(&t, &tmr);
	}

	if (stm == NULL) { /* invalid date? */
		return luaL_error(L, "date result cannot be represented in this installation");
	}

	if (strcmp(s, "*t") == 0) {
		lua_createtable(L, 0, 9); /* 9 = number of fields */
		luaext_oslib_setallfields(L, stm);
	} else {
		char cc[4]; /* buffer for individual conversion specifiers */
		luaL_Buffer b;

		cc[0] = '%';
		luaL_buffinit(L, &b);

		while (s < se) {
			if (*s != '%') { /* not a conversion specifier? */
				luaL_addchar(&b, *s++);
			} else {
				size_t reslen;
				char *buff;

				/*
				 * strftime is a C loop the interrupt hook cannot reach into,
				 * and one specifier can turn two input bytes into SIZETIMEFMT
				 * output bytes. Checking here is what bounds a format string
				 * made entirely of '%c'.
				 */
				LUAEXT_CHECK(L);

				buff = luaL_prepbuffsize(&b, SIZETIMEFMT);
				s++; /* skip '%' */

				/* copy specifier to 'cc' */
				s = luaext_oslib_checkoption(L, s, (size_t)(se - s), cc + 1);
				reslen = strftime(buff, SIZETIMEFMT, cc, stm);
				luaL_addsize(&b, reslen);

				luaext_oslib_check_result_length(L, luaL_bufflen(&b));
			}
		}

		/* The literal-copy path above cannot amplify -- it is bounded by the
		 * format string, which is already a Lua string -- so one check after
		 * the loop covers it. */
		luaext_oslib_check_result_length(L, luaL_bufflen(&b));

		luaL_pushresult(&b);
	}

	return 1;
}

static int luaext_oslib_time(lua_State *L)
{
	time_t t;

	if (lua_isnoneornil(L, 1)) { /* called without args? */
		t = time(NULL);			 /* get current time */
	} else {
		struct tm ts;

		luaL_checktype(L, 1, LUA_TTABLE);
		lua_settop(L, 1); /* make sure table is at the top */

		ts.tm_year = luaext_oslib_getfield(L, "year", -1, 1900);
		ts.tm_mon = luaext_oslib_getfield(L, "month", -1, 1);
		ts.tm_mday = luaext_oslib_getfield(L, "day", -1, 0);
		ts.tm_hour = luaext_oslib_getfield(L, "hour", 12, 0);
		ts.tm_min = luaext_oslib_getfield(L, "min", 0, 0);
		ts.tm_sec = luaext_oslib_getfield(L, "sec", 0, 0);
		ts.tm_isdst = luaext_oslib_getboolfield(L, "isdst");

		t = mktime(&ts);

		luaext_oslib_setallfields(L, &ts); /* update fields with normalized values */
	}

	if (t != (time_t)(l_timet)t || t == (time_t)(-1)) {
		return luaL_error(L, "time result cannot be represented in this installation");
	}

	l_pushtime(L, t);

	return 1;
}

static int luaext_oslib_difftime(lua_State *L)
{
	time_t t1 = luaext_oslib_checktime(L, 1);
	time_t t2 = luaext_oslib_checktime(L, 2);

	lua_pushnumber(L, (lua_Number)difftime(t1, t2));

	return 1;
}

/* -------------------------------------------------------------------------
 * Installation
 * ---------------------------------------------------------------------- */

/* -------------------------------------------------------------------------
 * os.remove / os.rename
 *
 * The two file operations upstream's os library carries. They route through
 * exactly the same layer io.open does -- canonicalisation, quota, the VfsError
 * allowlist -- rather than reaching the backend on their own, so there is one
 * place where a path becomes a name the host sees and one place where a refusal
 * is told apart from a failure.
 *
 * Absent without the vfs capability, and absent without vfsWrite too: both
 * modify the store, and read access must not imply the ability to delete.
 * ---------------------------------------------------------------------- */

static int luaext_oslib_remove(lua_State *L)
{
	luaext_sandbox *sandbox = LUAEXT_SB(L);
	zend_string *path = luaext_vfs_path_from_lua(L, sandbox, 1);
	zend_string *refusal = NULL;
	zval args[1];
	zval result;

	if (path == NULL) {
		return lua_error(L);
	}

	ZVAL_STR(&args[0], path);

	if (luaext_vfs_call(L, sandbox, "delete", 1, args, &result, &refusal) != LUAEXT_VFS_OK) {
		zend_string_release(path);

		if (refusal == NULL) {
			return lua_error(L);
		}

		lua_pushnil(L);
		lua_pushlstring(L, ZSTR_VAL(refusal), ZSTR_LEN(refusal));
		zend_string_release(refusal);

		return 2;
	}

	zend_string_release(path);
	zval_ptr_dtor(&result);

	lua_pushboolean(L, 1);

	return 1;
}

static int luaext_oslib_rename(lua_State *L)
{
	luaext_sandbox *sandbox = LUAEXT_SB(L);
	zend_string *from = luaext_vfs_path_from_lua(L, sandbox, 1);
	zend_string *to;
	zend_string *refusal = NULL;
	zval args[2];
	zval result;

	if (from == NULL) {
		return lua_error(L);
	}

	to = luaext_vfs_path_from_lua(L, sandbox, 2);

	if (to == NULL) {
		zend_string_release(from);
		return lua_error(L);
	}

	ZVAL_STR(&args[0], from);
	ZVAL_STR(&args[1], to);

	if (luaext_vfs_call(L, sandbox, "rename", 2, args, &result, &refusal) != LUAEXT_VFS_OK) {
		zend_string_release(from);
		zend_string_release(to);

		if (refusal == NULL) {
			return lua_error(L);
		}

		lua_pushnil(L);
		lua_pushlstring(L, ZSTR_VAL(refusal), ZSTR_LEN(refusal));
		zend_string_release(refusal);

		return 2;
	}

	zend_string_release(from);
	zend_string_release(to);
	zval_ptr_dtor(&result);

	lua_pushboolean(L, 1);

	return 1;
}

bool luaext_oslib_install(lua_State *L, luaext_sandbox *sandbox)
{
	luaext_oslib_clock_state *clock_state;

	luaL_checkstack(L, 8, "luaext: no stack to build the os library");

	lua_createtable(L, 0, 5);

	/*
	 * Unconditional. os.clock reports this sandbox's own billed CPU, which is
	 * the same quantity the CPU limit enforces -- a script timing itself
	 * measures exactly what will stop it, and can see nothing else.
	 */
	clock_state = (luaext_oslib_clock_state *)lua_newuserdatauv(L, sizeof(*clock_state), 0);
	clock_state->last_reported = 0.0;
	lua_pushcclosure(L, luaext_oslib_clock, 1);
	lua_setfield(L, -2, "clock");

	if (luaext_has_cap(&sandbox->policy, LUAEXT_CAP_OS_TIME)) {
		lua_pushcfunction(L, luaext_oslib_time);
		lua_setfield(L, -2, "time");

		lua_pushcfunction(L, luaext_oslib_date);
		lua_setfield(L, -2, "date");

		lua_pushcfunction(L, luaext_oslib_difftime);
		lua_setfield(L, -2, "difftime");
	}

	if (luaext_has_cap(&sandbox->policy, LUAEXT_CAP_OS_ENV)) {
		luaext_oslib_push_env_allow_list(L, sandbox);
		lua_pushcclosure(L, luaext_oslib_getenv, 1);
		lua_setfield(L, -2, "getenv");
	}

	/* Both need vfsWrite, not merely vfs: deleting and renaming are how a
	 * script destroys what the host stored, and a read grant must not carry
	 * that. */
	if (luaext_vfs_writable(sandbox)) {
		lua_pushcfunction(L, luaext_oslib_remove);
		lua_setfield(L, -2, "remove");

		lua_pushcfunction(L, luaext_oslib_rename);
		lua_setfield(L, -2, "rename");
	}

	lua_setglobal(L, LUA_OSLIBNAME);

	return true;
}
