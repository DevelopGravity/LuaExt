/*
 * luaext — the base library a sandbox sees.
 *
 * Owns the replacements for pcall, xpcall, print, collectgarbage, load and warn,
 * plus the warning hook, plus the allow-list the installer selects with.
 *
 * pcall/xpcall are the linchpin of the whole extension: a limit that a script
 * can catch is not a limit. Two things the implementation must get right, both
 * of which are easy to miss:
 *
 *   The test is on the lua_pcallk STATUS as well as the error value. A refused
 *   allocation raises LUA_ERRMEM carrying Lua's own preallocated string, not
 *   our userdata, so luaext_error_is_fatal() cannot see it -- and without the
 *   status check a script can pcall its way straight past memoryBytes.
 *
 *   xpcall's message handler is SKIPPED for a fatal, not run-then-rethrown. A
 *   handler's return value becomes the error object, so one line of Lua
 *   (`xpcall(f, function() return "oops" end)`) would replace the unforgeable
 *   marker with a plain string that every outer pcall then treats as catchable.
 */

#include "luaext_openlibs.h"

#include "luaext_error.h"
#include "luaext_output.h"

#include <lauxlib.h>
#include <lualib.h>

#include <string.h>

/* Stack slots any one step in here needs. */
#define LUAEXT_BASELIB_SLOTS 8

/*
 * What warn() puts in front of a message.
 *
 * A sandbox has one sink, so a warning and a print land in the same place;
 * upstream's wording is reused so that a host reading the output recognises it
 * as the same thing Lua would have written to stderr.
 */
#define LUAEXT_BASELIB_WARN_PREFIX "Lua warning: "

/* -------------------------------------------------------------------------
 * The allow list
 *
 * dofile and loadfile are withheld at every capability level: they open real
 * files by name, and the sandbox's only filesystem is the VFS.
 *
 * load is the compileAtRuntime capability. It is deleted unconditionally by the
 * placeholder this file replaces, which is why granting that capability does
 * nothing at all today.
 * ---------------------------------------------------------------------- */

const luaext_member luaext_baselib_allow[] = {
	{"_G", 0},
	{"_VERSION", 0},
	{"assert", 0},
	{"collectgarbage", 0},
	{"error", 0},
	{"getmetatable", 0},
	{"ipairs", 0},
	{"load", LUAEXT_CAP_COMPILE_AT_RUNTIME},
	{"next", 0},
	{"pairs", 0},
	{"pcall", 0},
	{"print", 0},
	{"rawequal", 0},
	{"rawget", 0},
	{"rawlen", 0},
	{"rawset", 0},
	{"select", 0},
	{"setmetatable", 0},
	{"tonumber", 0},

	/*
	 * Deliberately NOT replaced. Patch 0006 already hides the address in
	 * luaL_tolstring(), which tostring() is three lines from -- and the same
	 * patch covers print, string.format("%s", t), type-error messages and
	 * luaL_traceback. A replacement here would be a second stringification path
	 * that can only ever diverge from the one those four already share.
	 */
	{"tostring", 0},

	{"type", 0},
	{"warn", LUAEXT_CAP_WARN},
	{"xpcall", 0},
	{NULL, 0},
};

const char *const luaext_baselib_withheld[] = {
	"dofile",
	"loadfile",
	NULL,
};

/* -------------------------------------------------------------------------
 * pcall and xpcall
 * ---------------------------------------------------------------------- */

/*
 * What both protected calls do with a result.
 *
 * `base` is the number of stack slots that were already there before the call's
 * results, exactly as upstream's finishpcall uses it.
 */
static int luaext_baselib_finish(lua_State *L, int status, lua_KContext base)
{
	if (status == LUA_OK || status == LUA_YIELD) {
		return lua_gettop(L) - (int)base;
	}

	/*
	 * THE line this whole file exists for.
	 *
	 * A refused allocation is raised by Lua itself, carrying its own preallocated
	 * "not enough memory" string rather than one of our userdata, so
	 * luaext_error_is_fatal() answers false for it and the error-value test alone
	 * would let a script pcall straight past Limits::$memoryBytes.
	 *
	 * Re-raising the string would not be enough either: the enclosing protected
	 * call would then see LUA_ERRRUN and a plain string, and a NESTED pcall would
	 * catch what this one refused. So it is converted into the unforgeable fatal
	 * marker, which every pcall above -- ours and the extension's own -- already
	 * knows it must not swallow. Building the marker allocates, and if that
	 * allocation is refused too then Lua raises LUA_ERRMEM again, into the next
	 * protected call out, where this same test runs. The chain holds either way.
	 */
	if (status == LUA_ERRMEM) {
		lua_pop(L, 1);
		luaext_error_raise(L, LUAEXT_ERR_MEMORY, true,
						   "The sandbox is out of memory; a script may not catch its own memory "
						   "limit being reached");
	}

	if (luaext_error_is_fatal(L, -1)) {
		return lua_error(L);
	}

	/*
	 * LUA_ERRERR lands here, and belongs here. It can only arise from the
	 * script's own message handler failing while handling a CATCHABLE error --
	 * our trampoline never runs the handler for a fatal -- and upstream's
	 * `false, "error in error handling"` is the right answer to that.
	 */
	lua_pushboolean(L, 0);
	lua_pushvalue(L, -2);

	return 2;
}

static int luaext_baselib_pcall(lua_State *L)
{
	int status;

	luaL_checkany(L, 1);

	lua_pushboolean(L, 1); /* the first result, if nothing goes wrong */
	lua_insert(L, 1);

	/*
	 * lua_pcallk rather than lua_pcall, and a continuation, so that a coroutine
	 * can still yield across a pcall the way upstream's does. Dropping that would
	 * be a language change smuggled in as a security fix.
	 */
	status = lua_pcallk(L, lua_gettop(L) - 2, LUA_MULTRET, 0, 0, luaext_baselib_finish);

	return luaext_baselib_finish(L, status, 0);
}

/*
 * The message handler xpcall actually installs, closing over the script's.
 *
 * A handler's return value BECOMES the error object. So a handler that runs for
 * a fatal can replace the unforgeable marker with anything it likes -- a plain
 * string, say -- and every protected call further out would then treat a limit
 * breach as an ordinary catchable error. `xpcall(f, function() return "oops" end)`
 * is the whole attack, and it is one line.
 *
 * The fix is not to run the handler and then rethrow: by then the substitution
 * has already happened. The handler is skipped entirely, and the error value is
 * returned exactly as it arrived.
 *
 * Nothing in here allocates or raises before the delegation, which is what a
 * message handler must be able to promise: it runs on a stack that has already
 * failed once.
 *
 * The one visible cost is a frame. `xpcall(f, debug.traceback)` now runs the
 * script's handler one C frame further out than upstream would, so a traceback
 * taken inside it carries an extra "[C]" line. That is the price of deciding
 * fatality before the handler sees the value, and it is the right way round: a
 * cosmetic frame against a marker that can be laundered.
 */
static int luaext_baselib_xpcall_handler(lua_State *L)
{
	lua_settop(L, 1);

	if (luaext_error_is_fatal(L, 1)) {
		return 1;
	}

	lua_pushvalue(L, lua_upvalueindex(1));
	lua_pushvalue(L, 1);
	lua_call(L, 1, 1);

	return 1;
}

static int luaext_baselib_xpcall(lua_State *L)
{
	int argc = lua_gettop(L);
	int status;

	luaL_checktype(L, 2, LUA_TFUNCTION);
	luaL_checkstack(L, LUAEXT_BASELIB_SLOTS, "luaext: no stack to protect a call");

	/* The script's handler becomes an upvalue of ours, and ours takes its place
	 * as the message handler lua_pcallk is given. */
	lua_pushvalue(L, 2);
	lua_pushcclosure(L, luaext_baselib_xpcall_handler, 1);
	lua_replace(L, 2);

	lua_pushboolean(L, 1); /* the first result, if nothing goes wrong */
	lua_pushvalue(L, 1);   /* the function to call */
	lua_rotate(L, 3, 2);   /* both below its arguments */

	status = lua_pcallk(L, argc - 2, LUA_MULTRET, 2, 2, luaext_baselib_finish);

	return luaext_baselib_finish(L, status, 2);
}

/* -------------------------------------------------------------------------
 * print
 * ---------------------------------------------------------------------- */

/*
 * One print is one write.
 *
 * Buffered rather than written argument by argument because the sink is a budget
 * and a callback, not a stream: a host that asked for streaming output should
 * see the line a script printed, not each of its fields and the tabs between
 * them as separate calls.
 *
 * luaL_tolstring is what upstream uses too, and it is what patch 0006 fixed: a
 * table prints as "table: (address hidden)" rather than as a heap address.
 */
static int luaext_baselib_print(lua_State *L)
{
	luaext_sandbox *sandbox = LUAEXT_SB(L);
	int argc = lua_gettop(L);
	int index;
	luaL_Buffer buffer;
	const char *line;
	size_t length = 0;

	luaL_checkstack(L, LUAEXT_BASELIB_SLOTS, "luaext: no stack to print");
	luaL_buffinit(L, &buffer);

	for (index = 1; index <= argc; index++) {
		if (index > 1) {
			luaL_addchar(&buffer, '\t');
		}

		/* May run a __tostring metamethod, which is arbitrary script code; it
		 * runs before anything is handed to the sink, so a failure there costs
		 * nothing but the buffer. */
		luaL_tolstring(L, index, NULL);
		luaL_addvalue(&buffer);
	}

	luaL_addchar(&buffer, '\n');
	luaL_pushresult(&buffer);

	line = lua_tolstring(L, -1, &length);

	/*
	 * The write is the last thing this frame does that can fail, and it owns
	 * nothing when it happens: the buffer has already collapsed into the string
	 * on the stack, and there is no zval or emalloc'd block here at all. That
	 * matters twice over -- false means the budget is spent and the host asked to
	 * fail rather than truncate, so this raises on the spot; and the sink itself
	 * may unwind, because in Callback mode it calls a host callback that can
	 * throw. Either way nothing is stranded, and the traceback names the print
	 * that overran.
	 */
	if (!luaext_output_write(sandbox, line, length)) {
		luaext_error_raise(L, LUAEXT_ERR_OUTPUT, true,
						   "The sandbox has written all the output it is allowed");
	}

	lua_pop(L, 1);

	return 0;
}

/* -------------------------------------------------------------------------
 * warn
 * ---------------------------------------------------------------------- */

/*
 * Where every warning the INTERPRETER raises on its own initiative ends up:
 * nowhere.
 *
 * Installed unconditionally, so "nothing this extension runs reaches stderr" is
 * a property of the sandbox rather than of which libraries it happened to open.
 * Lua's default warning function writes to stderr, and the interpreter warns by
 * itself -- an error inside a __gc metamethod is reported exactly this way.
 *
 * It discards rather than forwards, at every capability level, for two reasons
 * that point the same way:
 *
 *   It cannot safely write. luaE_warnerror() is called from luaC_GCTM OUTSIDE
 *   the protected call that ran the finaliser, and from lua_close() where there
 *   is no protected call at all -- and luaext_output_write() can unwind, because
 *   a Callback-mode sink invokes a host callback that may throw. An unwind from
 *   either site would longjmp out of the collector.
 *
 *   It should not spend the budget. Forwarding finaliser diagnostics would let a
 *   script consume its output allowance from a finaliser, somewhere it cannot
 *   even be told that it has.
 *
 * The cost is real and deliberate: a __gc that fails is silent. warn() itself is
 * unaffected -- it writes to the sink directly, from a frame where raising is
 * safe.
 */
static void luaext_baselib_warnf(void *ud, const char *message, int to_continue)
{
	(void)ud;
	(void)message;
	(void)to_continue;
}

void luaext_baselib_install_warnf(lua_State *L, luaext_sandbox *sandbox)
{
	lua_setwarnf(L, luaext_baselib_warnf, sandbox);
}

/*
 * Ours rather than upstream's, and it does not go through lua_warning().
 *
 * Upstream's warn() emits each argument as a separate continuation part of one
 * message, which the warning function then has to reassemble -- and the warning
 * function is precisely where writing is not safe. Composing here instead means
 * one warn() is one write, from a frame that owns nothing when it makes it, and
 * that the '@' control test sees the message a script actually wrote rather than
 * its first fragment.
 */
static int luaext_baselib_warn(lua_State *L)
{
	luaext_sandbox *sandbox = LUAEXT_SB(L);
	int argc = lua_gettop(L);
	int index;
	luaL_Buffer buffer;
	const char *line;
	size_t length = 0;

	luaL_checkstring(L, 1); /* at least one argument, and all of them strings */

	for (index = 2; index <= argc; index++) {
		luaL_checkstring(L, index);
	}

	luaL_checkstack(L, LUAEXT_BASELIB_SLOTS, "luaext: no stack to warn");
	luaL_buffinit(L, &buffer);
	luaL_addstring(&buffer, LUAEXT_BASELIB_WARN_PREFIX);

	for (index = 1; index <= argc; index++) {
		lua_pushvalue(L, index);
		luaL_addvalue(&buffer);
	}

	luaL_addchar(&buffer, '\n');
	luaL_pushresult(&buffer);

	line = lua_tolstring(L, -1, &length);

	/*
	 * A control message, in Lua's sense: one beginning with '@'. Upstream's own
	 * warning function uses "@on" and "@off" to switch itself; there is nothing
	 * here to switch, and forwarding them would emit the control word as though a
	 * script had asked to print it.
	 */
	if (length > sizeof(LUAEXT_BASELIB_WARN_PREFIX) - 1 &&
		line[sizeof(LUAEXT_BASELIB_WARN_PREFIX) - 1] == '@') {
		lua_pop(L, 1);
		return 0;
	}

	/* Same reasoning as print: nothing is owned here, so both the refusal and an
	 * unwind out of a Callback-mode sink are safe. */
	if (!luaext_output_write(sandbox, line, length)) {
		luaext_error_raise(L, LUAEXT_ERR_OUTPUT, true,
						   "The sandbox has written all the output it is allowed");
	}

	lua_pop(L, 1);

	return 0;
}

/* -------------------------------------------------------------------------
 * collectgarbage
 * ---------------------------------------------------------------------- */

/*
 * Verbs any sandbox may use.
 *
 * "collect" is on this list on purpose. A full collection only ever frees more
 * than it started with, and a script that wants to force one before measuring is
 * doing something reasonable; withholding it would make an adversarial test of a
 * default sandbox fail for no gain.
 */
static const char *const luaext_baselib_gc_open[] = {
	"collect", "count", "isrunning", "step", NULL,
};

/*
 * Verbs that need gcControl, and why they are more dangerous here than upstream.
 *
 * luaext_alloc_tune_gc() installs GC parameters in tiers as usage approaches
 * memoryBytes, and CACHES which tier is installed. One collectgarbage("param",
 * ...) overwrites what it installed without changing that cache, so the
 * allocator believes the tuning is still in place and never reinstalls it: GC
 * pressure tuning is then defeated for the whole life of that sandbox. "stop"
 * is the blunter version of the same thing.
 */
static const char *const luaext_baselib_gc_privileged[] = {
	"generational", "incremental", "param", "restart", "stop", NULL,
};

static bool luaext_baselib_gc_listed(const char *const *verbs, const char *option)
{
	int index;

	for (index = 0; verbs[index] != NULL; index++) {
		if (strcmp(verbs[index], option) == 0) {
			return true;
		}
	}

	return false;
}

/*
 * Upvalue 1 is upstream's collectgarbage.
 *
 * The option is checked here rather than by handing it to luaL_checkoption over
 * upstream's array, because that function's error message enumerates every
 * option it knows -- which would name the withheld verbs to the script that just
 * failed to use one.
 */
static int luaext_baselib_collectgarbage(lua_State *L)
{
	luaext_sandbox *sandbox = LUAEXT_SB(L);
	const char *option = luaL_optstring(L, 1, "collect");
	int argc;

	if (!luaext_baselib_gc_listed(luaext_baselib_gc_open, option)) {
		if (!luaext_baselib_gc_listed(luaext_baselib_gc_privileged, option)) {
			return luaL_argerror(L, 1, lua_pushfstring(L, "invalid option '%s'", option));
		}

		if (sandbox == NULL || !luaext_has_cap(&sandbox->policy, LUAEXT_CAP_GC_CONTROL)) {
			return luaL_error(L,
							  "collectgarbage(\"%s\") needs the gcControl capability, which this "
							  "sandbox was not given",
							  option);
		}
	}

	if (sandbox != NULL && strcmp(option, "collect") == 0) {
		sandbox->gc_collections++;
	}

	luaL_checkstack(L, LUAEXT_BASELIB_SLOTS, "luaext: no stack to collect garbage");

	argc = lua_gettop(L);
	lua_pushvalue(L, lua_upvalueindex(1));
	lua_insert(L, 1);
	lua_call(L, argc, LUA_MULTRET);

	return lua_gettop(L);
}

/* -------------------------------------------------------------------------
 * load
 * ---------------------------------------------------------------------- */

static size_t luaext_baselib_max_source(const luaext_sandbox *sandbox)
{
	return sandbox != NULL ? sandbox->policy.limits.max_source_bytes : 0;
}

/*
 * The reader a load() from a function actually gets, wrapping the script's.
 *
 * Upvalue 1 is the script's reader; upvalue 2 is a userdata holding the running
 * byte count. Without this, `load(function() ... end)` is a straight bypass of
 * Limits::$maxSourceBytes: the string form can be measured before the parser
 * sees it, and the reader form cannot.
 *
 * Overrunning raises rather than returning nil. Returning nil would end the
 * input, and load() would then happily compile whatever prefix the script had
 * already fed it -- silently compiling something nobody wrote, which is worse
 * than a refusal. The raise is an ordinary catchable Lua error: the script asked
 * for too much, which is its mistake to handle, and the limit is enforced either
 * way. It does mean load() can raise where upstream would have returned
 * `nil, message`; that is the price of delegating to upstream's loader rather
 * than reimplementing it.
 */
static int luaext_baselib_load_reader(lua_State *L)
{
	luaext_sandbox *sandbox = LUAEXT_SB(L);
	size_t *consumed = (size_t *)lua_touserdata(L, lua_upvalueindex(2));
	size_t max_source = luaext_baselib_max_source(sandbox);
	size_t length = 0;

	lua_settop(L, 0);
	lua_pushvalue(L, lua_upvalueindex(1));
	lua_call(L, 0, 1);

	if (lua_type(L, -1) != LUA_TSTRING || consumed == NULL || max_source == 0) {
		return 1;
	}

	(void)lua_tolstring(L, -1, &length);

	/* Written as a subtraction so it cannot overflow: *consumed never exceeds
	 * max_source, because this is what stops it. */
	if (length > max_source - *consumed) {
		return luaL_error(L,
						  "the chunk exceeds the %I byte source limit this sandbox was configured "
						  "with",
						  (lua_Integer)max_source);
	}

	*consumed += length;

	return 1;
}

/*
 * Upvalue 1 is upstream's load.
 *
 * Three gates, and none of them exist upstream:
 *
 *   Mode. Without loadBytecode the mode is forced to "t", so a binary chunk is a
 *   load error rather than an unverified jump into native code. Lua has no
 *   bytecode verifier and has never claimed one.
 *
 *   Source size. Sandbox::compile() enforces Limits::$maxSourceBytes and
 *   upstream's load does not, so granting compileAtRuntime currently opens a
 *   bypass of a limit that exists precisely because the parser is the one phase
 *   no interrupt can land in.
 *
 *   Reader output, capped the same way -- see luaext_baselib_load_reader.
 */
static int luaext_baselib_load(lua_State *L)
{
	luaext_sandbox *sandbox = LUAEXT_SB(L);
	size_t max_source = luaext_baselib_max_source(sandbox);
	bool allow_binary =
		sandbox != NULL && luaext_has_cap(&sandbox->policy, LUAEXT_CAP_LOAD_BYTECODE);
	int argc = lua_gettop(L);
	const char *chunk;
	size_t length = 0;

	luaL_checkstack(L, LUAEXT_BASELIB_SLOTS, "luaext: no stack to load a chunk");

	if (!allow_binary) {
		/*
		 * Padding with nil rather than settop(3): load()'s fourth argument is an
		 * environment, and it is distinguished by lua_isnone rather than by being
		 * nil, so materialising a fourth argument would set _ENV to nil on a
		 * three-argument call.
		 */
		while (lua_gettop(L) < 2) {
			lua_pushnil(L);
		}

		lua_pushliteral(L, "t");

		if (argc >= 3) {
			lua_replace(L, 3);
		}
	}

	chunk = lua_tolstring(L, 1, &length);

	if (chunk != NULL) {
		if (max_source != 0 && length > max_source) {
			luaL_pushfail(L);
			lua_pushfstring(L,
							"the chunk is %I bytes, which exceeds the %I byte source limit this "
							"sandbox was configured with",
							(lua_Integer)length, (lua_Integer)max_source);
			return 2;
		}
	} else if (max_source != 0 && lua_type(L, 1) == LUA_TFUNCTION) {
		size_t *consumed;

		lua_pushvalue(L, 1);
		consumed = (size_t *)lua_newuserdatauv(L, sizeof(*consumed), 0);
		*consumed = 0;
		lua_pushcclosure(L, luaext_baselib_load_reader, 2);
		lua_replace(L, 1);
	}

	argc = lua_gettop(L);
	lua_pushvalue(L, lua_upvalueindex(1));
	lua_insert(L, 1);
	lua_call(L, argc, LUA_MULTRET);

	/*
	 * load() is a protected call in disguise, and the invariant that governs
	 * every one of them applies here too.
	 *
	 * lua_load runs the parser under luaD_protectedparser, and the reader is
	 * script code -- it can call a host callback, and that callback can fail.
	 * Upstream then reports the failure as load()'s ordinary `fail, message`
	 * return, which hands the script a fatal error as a value it may ignore.
	 * Re-raised here instead.
	 *
	 * The one case this cannot see is a refused ALLOCATION inside the parser:
	 * Lua raises that with its own preallocated string and no status survives
	 * the return, so `load(huge)` still answers `fail, "not enough memory"`
	 * where pcall would have re-raised. Closing that properly wants a latched
	 * breach flag on the sandbox that every protected boundary consults, which
	 * is a change to the allocator and error subsystems rather than to this one.
	 */
	if (lua_gettop(L) == 2 && lua_isnil(L, 1) && luaext_error_is_fatal(L, 2)) {
		lua_remove(L, 1);
		return lua_error(L);
	}

	return lua_gettop(L);
}

/* -------------------------------------------------------------------------
 * Assembly
 * ---------------------------------------------------------------------- */

/* Move the value on top of the stack into the selected table under `name`. */
static void luaext_baselib_put(lua_State *L, int selected, const char *name)
{
	lua_pushstring(L, name);
	lua_insert(L, -2);
	lua_rawset(L, selected);
}

/* Push the upstream member `name` out of the scratch table. */
static void luaext_baselib_original(lua_State *L, int scratch, const char *name)
{
	lua_pushstring(L, name);
	lua_rawget(L, scratch);
}

bool luaext_baselib_decorate(lua_State *L, luaext_sandbox *sandbox)
{
	int scratch = lua_absindex(L, -2);
	int selected = lua_absindex(L, -1);

	luaL_checkstack(L, LUAEXT_BASELIB_SLOTS, "luaext: no stack to assemble the base library");

	lua_pushcfunction(L, luaext_baselib_pcall);
	luaext_baselib_put(L, selected, "pcall");

	lua_pushcfunction(L, luaext_baselib_xpcall);
	luaext_baselib_put(L, selected, "xpcall");

	lua_pushcfunction(L, luaext_baselib_print);
	luaext_baselib_put(L, selected, "print");

	luaext_baselib_original(L, scratch, "collectgarbage");
	lua_pushcclosure(L, luaext_baselib_collectgarbage, 1);
	luaext_baselib_put(L, selected, "collectgarbage");

	/*
	 * Only where selection put one there. Installing a wrapper for a member the
	 * allow list withheld would put it back, which is the failure mode an
	 * allow-list exists to make impossible.
	 */
	if (luaext_has_cap(&sandbox->policy, LUAEXT_CAP_COMPILE_AT_RUNTIME)) {
		luaext_baselib_original(L, scratch, "load");
		lua_pushcclosure(L, luaext_baselib_load, 1);
		luaext_baselib_put(L, selected, "load");
	}

	if (luaext_has_cap(&sandbox->policy, LUAEXT_CAP_WARN)) {
		lua_pushcfunction(L, luaext_baselib_warn);
		luaext_baselib_put(L, selected, "warn");
	}

	return true;
}
