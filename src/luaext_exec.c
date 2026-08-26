/*
 * luaext — running Lua and reading its results.
 *
 * Two rules shape this file.
 *
 * The first is that every entry into the interpreter is protected and carries
 * the traceback handler. A script is untrusted, so an error it raises has to
 * arrive back here as a status code rather than as a longjmp past a PHP method
 * body, and it has to arrive with the stack it failed on already recorded --
 * that stack no longer exists by the time the host asks about it.
 *
 * The second is that nothing here indexes a table through a metamethod. Reading
 * a global, walking a dotted path and converting a result all happen after the
 * script has nominally finished, and running its __index there would hand
 * control back to untrusted code outside the call that was bounding it. Every
 * lookup below is raw for that reason, which is the same reason the conversion
 * subsystem walks tables raw.
 *
 * The stack contract is written on each function and holds on both paths: a
 * failure leaves the interpreter exactly as it was found, so a refused
 * conversion costs a value and never a usable sandbox.
 */

#include "luaext_exec.h"

#include "luaext_convert.h"
#include "luaext_corolib.h"
#include "luaext_defer.h"
#include "luaext_error.h"
#include "luaext_timers.h"

#include <lauxlib.h>
#include <lua.h>

#include <limits.h>
#include <string.h>

#include <Zend/zend_exceptions.h>

/*
 * Bytes of a dotted path echoed into an error message.
 *
 * A path is host-supplied rather than script-supplied, so this is not a
 * disclosure boundary; it exists because the raise buffer is fixed and a
 * precision argument to "%.*s" is an int.
 */
#define LUAEXT_EXEC_PATH_SHOWN 96

/* -------------------------------------------------------------------------
 * Shared guards
 * ---------------------------------------------------------------------- */

/*
 * Openness is a property of the sandbox's main state, so this asks about that
 * one; which state the work then runs on is luaext_exec_state()'s question.
 */
static bool luaext_exec_ready(const luaext_sandbox *sandbox)
{
	if (sandbox == NULL || sandbox->closed || sandbox->L == NULL) {
		zend_throw_exception(luaext_ce_closed_sandbox_error, "The sandbox has been closed", 0);
		return false;
	}

	return true;
}

/*
 * The state this call should run on: whichever one is executing, not always the
 * main thread.
 *
 * Outside a coroutine these are the same object and nothing changes. Inside one
 * they differ, and the difference matters. A host callback invoked from a
 * coroutine runs on THAT coroutine's C stack, so when it calls back into Lua the
 * re-entrant call belongs there too -- a C function calling lua_pcall on the
 * state it was itself called from is the ordinary, documented pattern.
 *
 * Using the main thread instead would push a call onto a state that is sitting
 * inside lua_resume, and lua_resume transfers the C-call budget with
 * `L->nCcalls = getCcalls(from)`. Spending more of that budget on `from` while
 * the resume is outstanding makes the accounting that guards against a genuine
 * C-stack overflow wrong, which turns a Lua error into a crash. Lua's API
 * checker does not object to it, which is precisely why it is worth writing
 * down rather than leaving to be rediscovered.
 *
 * running_L is set at construction and maintained by the coroutine wrapper
 * around every resume, so it is never null for an open sandbox.
 */
lua_State *luaext_exec_state(const luaext_sandbox *sandbox)
{
	return sandbox->running_L != NULL ? sandbox->running_L : sandbox->L;
}

static int luaext_exec_shown(size_t length)
{
	return length > LUAEXT_EXEC_PATH_SHOWN ? LUAEXT_EXEC_PATH_SHOWN : (int)length;
}

/* -------------------------------------------------------------------------
 * Loading
 * ---------------------------------------------------------------------- */

bool luaext_exec_load(luaext_sandbox *sandbox, const char *code, size_t code_len,
					  const char *chunk_name, bool allow_binary)
{
	lua_State *L;
	size_t max_source;
	int status;

	if (!luaext_exec_ready(sandbox)) {
		return false;
	}

	L = luaext_exec_state(sandbox);
	max_source = sandbox->policy.limits.max_source_bytes;

	/*
	 * Before the parser sees a byte of it. Parsing is the one phase no
	 * interrupt can land in -- the hook that stops a runaway script only runs
	 * between instructions of a chunk that already compiled -- so a
	 * pathological source is bounded by its length or not at all. Truncating to
	 * fit would compile something the caller did not write, which is worse than
	 * refusing.
	 */
	if (max_source != 0 && code_len > max_source) {
		zend_throw_exception_ex(luaext_ce_syntax_error, 0,
								"The chunk is %zu bytes, which exceeds the %zu byte source limit "
								"this sandbox was configured with",
								code_len, max_source);
		return false;
	}

	/* Not a SyntaxError: nothing is wrong with the chunk, and saying otherwise
	 * would send whoever reads the log looking at the wrong thing. */
	if (!lua_checkstack(L, 4)) {
		zend_throw_exception(luaext_ce_memory_limit_error,
							 "Cannot compile a chunk: the interpreter stack cannot grow", 0);
		return false;
	}

	/*
	 * The security boundary of the whole extension sits in this one argument.
	 * Lua has no bytecode verifier and has never claimed one: a crafted binary
	 * chunk is arbitrary native execution, not a parse error. "t" makes that
	 * unreachable from a string an untrusted caller supplied; "bt" is only
	 * reached once the caller has checked the loadBytecode capability.
	 */
	status = luaL_loadbufferx(L, code, code_len, chunk_name, allow_binary ? "bt" : "t");

	if (status != LUA_OK) {
		/* Pops the message the loader pushed, so the stack is as we found it. */
		luaext_error_throw_from_lua(sandbox, L, status);
		return false;
	}

	return true;
}

/* -------------------------------------------------------------------------
 * Pushing PHP values under protection
 * ---------------------------------------------------------------------- */

typedef struct {
	zval *values;
	uint32_t count;
} luaext_exec_values;

/* Argument 1 is the descriptor; every converted value comes back as a result. */
static int luaext_exec_push_trampoline(lua_State *L)
{
	const luaext_exec_values *request = (const luaext_exec_values *)lua_touserdata(L, 1);
	uint32_t index;

	lua_settop(L, 0);

	if (!lua_checkstack(L, (int)request->count + LUA_MINSTACK)) {
		luaext_error_raise(L, LUAEXT_ERR_CONVERSION, true, "%s",
						   "Cannot convert PHP values to Lua: the interpreter stack cannot grow "
						   "far enough to hold them");
	}

	for (index = 0; index < request->count; index++) {
		luaext_convert_push_zval(LUAEXT_SB(L), L, &request->values[index]);
	}

	return (int)request->count;
}

/*
 * Convert `count` PHP values onto the stack, leaving exactly that many on
 * success and nothing on failure.
 *
 * Deliberately a separate protected call from the one that runs the function.
 * Folding the two together would leave this frame sitting underneath the
 * script's own, and every traceback the host ever sees would carry a C frame
 * from the plumbing beneath its main chunk.
 */
static bool luaext_exec_push_values(luaext_sandbox *sandbox, zval *values, uint32_t count)
{
	luaext_exec_values request;
	lua_State *L;
	int top;
	int handler;
	int status;

	if (!luaext_exec_ready(sandbox)) {
		return false;
	}

	L = luaext_exec_state(sandbox);
	top = lua_gettop(L);

	if (count > (uint32_t)INT_MAX - LUA_MINSTACK || !lua_checkstack(L, (int)count + LUA_MINSTACK)) {
		zend_throw_exception(luaext_ce_conversion_error,
							 "Cannot convert PHP values to Lua: the interpreter stack cannot grow",
							 0);
		return false;
	}

	request.values = values;
	request.count = count;

	lua_pushcfunction(L, luaext_error_traceback_handler);
	handler = lua_gettop(L);

	lua_pushcfunction(L, luaext_exec_push_trampoline);
	lua_pushlightuserdata(L, &request);

	status = lua_pcall(L, 1, (int)count, handler);

	if (status != LUA_OK) {
		luaext_error_throw_from_lua(sandbox, L, status);
		lua_settop(L, top);
		return false;
	}

	/* Leaves only the converted values where the caller expects them. */
	lua_remove(L, handler);

	return true;
}

bool luaext_exec_push_value(luaext_sandbox *sandbox, zval *value)
{
	return luaext_exec_push_values(sandbox, value, 1);
}

/* -------------------------------------------------------------------------
 * Calling
 * ---------------------------------------------------------------------- */

bool luaext_exec_pcall(luaext_sandbox *sandbox, int func_index, zval *args, uint32_t argc,
					   zval *return_value)
{
	luaext_watch_frame frame;
	lua_State *L;
	int base;
	int handler;
	int status;
	int outer_no_raise_depth;
	bool converted;
	bool interrupted;

	if (!luaext_exec_ready(sandbox)) {
		return false;
	}

	L = luaext_exec_state(sandbox);
	func_index = lua_absindex(L, func_index);

	/* Everything at or above the function belongs to this call and goes with
	 * it, which is what "leaving nothing on the stack" means. */
	base = func_index - 1;

	if (!lua_checkstack(L, LUA_MINSTACK)) {
		lua_settop(L, base);
		zend_throw_exception(luaext_ce_conversion_error,
							 "Cannot call a Lua function: the interpreter stack cannot grow", 0);
		return false;
	}

	/*
	 * The handler goes on before the call, not after the failure: it runs while
	 * the erroring stack still exists, and that stack is the only thing that can
	 * say where the script went wrong.
	 */
	lua_pushcfunction(L, luaext_error_traceback_handler);
	handler = lua_gettop(L);

	lua_pushvalue(L, func_index);

	if (argc > 0 && !luaext_exec_push_values(sandbox, args, argc)) {
		lua_settop(L, base);
		return false;
	}

	sandbox->lua_calls_in++;

	/*
	 * The one bracket that arms the timing limits. It also owns in_lua, because
	 * only the OUTERMOST entry may arm and the depth is how that is known: a
	 * nested call made from inside a host callback must not restart the clock.
	 */
	luaext_timers_enter_lua(sandbox, &frame);

	/*
	 * Entering the interpreter starts a NEW frame for the no-raise discipline,
	 * so the outer frame's depth is set aside for the duration.
	 *
	 * That discipline says: do not raise while THIS frame still owns a zval or
	 * an allocation, because lua_error() longjmps past C cleanup. A nested entry
	 * is reached through a host callback, and luaext_phpcall_invoke holds the
	 * bracket across that callback because it owns the converted arguments --
	 * but a raise from inside this pcall unwinds only as far as this pcall, and
	 * cannot reach those arguments at all. Inheriting the outer depth would
	 * therefore report a violation that is not one, and the assertion would be
	 * describing something untrue rather than protecting anything.
	 */
	outer_no_raise_depth = sandbox->no_raise_depth;
	sandbox->no_raise_depth = 0;

	status = lua_pcall(L, (int)argc, LUA_MULTRET, handler);

	sandbox->no_raise_depth = outer_no_raise_depth;

	/*
	 * Asked BEFORE leaving, because leaving is where the sticky interrupt flag
	 * is cleared. A call that came back with LUA_OK while a limit breach was
	 * still raised did not succeed -- something inside it caught the breach and
	 * carried on -- and its results are not results.
	 */
	/*
	 * INSIDE the bracket, and BEFORE the interrupt is examined. Both halves of
	 * that placement are load bearing.
	 *
	 * Inside, because closing a coroutine runs its <close> variables -- untrusted
	 * Lua -- and the outermost leave_lua below disarms the watchdog and clears
	 * the sticky interrupt flag. Sweeping after it would run those handlers
	 * unmetered and uninterruptible, so `while true do end` in a <close> body
	 * would hang the process: the denial of service luaext_timers_detach()
	 * already defends close() against.
	 *
	 * Before, because a handler that burns the remaining budget raises the flag
	 * during the sweep. Asking about the interrupt first would read it too early
	 * and report a clean return for a call that overran inside its own cleanup.
	 * lua_closethread's status is not consulted for this: it reports what one
	 * thread did, whereas the flag is what the whole call did.
	 */
	if (sandbox->in_lua == 1) {
		luaext_corolib_sweep(sandbox);
	}

	interrupted = status == LUA_OK && luaext_timers_throw_if_interrupted(sandbox);

	luaext_timers_leave_lua(sandbox, &frame);

	/*
	 * The routine drain point, and the reason it is here rather than deeper:
	 * this is where the outermost call has fully unwound, so no Lua execution is
	 * in progress and a __destruct released now cannot re-enter the collector it
	 * was queued from. See luaext_defer.h.
	 *
	 * Draining on every outermost return, not only at close, keeps the queue
	 * from growing across a long-lived sandbox's many calls.
	 */
	if (sandbox->in_lua == 0) {
		luaext_defer_drain(sandbox);
	}

	if (interrupted) {
		lua_settop(L, base);

		return false;
	}

	if (status != LUA_OK) {
		/*
		 * Classification stays in the error subsystem. It is the only place
		 * that knows a fatal error must not surface as a class the host would
		 * read as catchable, and duplicating that judgement here is how a limit
		 * breach eventually becomes a RuntimeError somebody swallows.
		 */
		luaext_error_throw_from_lua(sandbox, L, status);
		lua_settop(L, base);

		return false;
	}

	converted = luaext_convert_stack_to_array(sandbox, L, handler + 1, lua_gettop(L) - handler,
											  return_value);

	lua_settop(L, base);

	return converted;
}

/* -------------------------------------------------------------------------
 * Dotted paths
 *
 * "a.b.c" names c inside b inside a inside the globals table. Traversal is raw
 * for the reason given at the top of this file: a host reading a global must
 * not be a way to run script code at a moment nothing is bounding it.
 *
 * Neither of the two entry points below carries the luaext_timers_enter_lua
 * bracket, and that is a decision rather than an omission. Both run inside a
 * lua_pcall trampoline, so they do execute interpreter code -- but every lookup
 * is raw, no metamethod can run, and the work is bounded by the length of the
 * path. No timing limit can fire usefully inside one. Arming them would put a
 * lock acquisition and a clock read on every getGlobal() and setGlobal() to
 * bound something that cannot run away. The rawness is what makes that safe: if
 * a future change ever lets a metamethod run here, the bracket has to come back
 * with it.
 * ---------------------------------------------------------------------- */

typedef struct {
	const char *path;
	size_t len;
} luaext_exec_path;

/*
 * Reject a path that names nothing: "", "a.", ".a", "a..b".
 *
 * Checked before the interpreter is entered, because it is a mistake in the
 * calling PHP rather than anything the script did -- and because the walk below
 * would otherwise have to decide what an empty key means.
 *
 * Reported against argument 1, which is where the path sits in every method
 * that takes one.
 */
static bool luaext_exec_path_valid(const char *path, size_t path_len)
{
	size_t start = 0;
	size_t index;

	if (path_len == 0) {
		zend_argument_value_error(1, "must name a Lua global, optionally with dotted components");
		return false;
	}

	for (index = 0; index <= path_len; index++) {
		if (index == path_len || path[index] == '.') {
			if (index == start) {
				zend_argument_value_error(1, "must not contain an empty path component");
				return false;
			}

			start = index + 1;
		}
	}

	return true;
}

/*
 * An intermediate that exists but is not a table.
 *
 * Distinct from a missing one: `a.b.c` where a.b is nil describes a value that
 * is simply not there, while a.b being a number describes a path that cannot
 * mean anything. Answering the second with null would report absence where the
 * real answer is that the caller is asking the wrong question.
 */
ZEND_COLD ZEND_NORETURN static void
luaext_exec_path_refuse(lua_State *L, const luaext_exec_path *request, size_t prefix_len)
{
	luaext_error_raise(L, LUAEXT_ERR_RUNTIME, false,
					   "Cannot resolve the Lua path \"%.*s\": \"%.*s\" is a %s, which cannot be "
					   "indexed",
					   luaext_exec_shown(request->len), request->path,
					   luaext_exec_shown(prefix_len), request->path, luaL_typename(L, -1));
}

/* Argument 1 is the path descriptor; the value it names is the single result. */
static int luaext_exec_path_reader(lua_State *L)
{
	const luaext_exec_path *request = (const luaext_exec_path *)lua_touserdata(L, 1);
	size_t cursor = 0;

	lua_settop(L, 0);
	lua_pushglobaltable(L);

	for (;;) {
		const char *component = request->path + cursor;
		const char *dot = (const char *)memchr(component, '.', request->len - cursor);
		size_t component_len = dot != NULL ? (size_t)(dot - component) : request->len - cursor;

		/*
		 * Nothing below a missing table exists either, so the walk stops with
		 * nil rather than asking the interpreter to index it. That is what makes
		 * getGlobal("a.b.c") null for an unset `a` instead of an error.
		 */
		if (lua_isnil(L, -1)) {
			break;
		}

		if (!lua_istable(L, -1)) {
			luaext_exec_path_refuse(L, request, cursor > 0 ? cursor - 1 : 0);
		}

		lua_pushlstring(L, component, component_len);
		lua_rawget(L, -2);
		lua_remove(L, -2);

		if (dot == NULL) {
			break;
		}

		cursor += component_len + 1;
	}

	return 1;
}

bool luaext_exec_push_path(luaext_sandbox *sandbox, const char *path, size_t path_len)
{
	luaext_exec_path request;
	lua_State *L;
	int top;
	int handler;
	int status;

	if (!luaext_exec_ready(sandbox) || !luaext_exec_path_valid(path, path_len)) {
		return false;
	}

	L = luaext_exec_state(sandbox);
	top = lua_gettop(L);

	if (!lua_checkstack(L, 8)) {
		zend_throw_exception(luaext_ce_runtime_error,
							 "Cannot read a Lua global: the interpreter stack cannot grow", 0);
		return false;
	}

	request.path = path;
	request.len = path_len;

	lua_pushcfunction(L, luaext_error_traceback_handler);
	handler = lua_gettop(L);

	lua_pushcfunction(L, luaext_exec_path_reader);
	lua_pushlightuserdata(L, &request);

	status = lua_pcall(L, 1, 1, handler);

	if (status != LUA_OK) {
		luaext_error_throw_from_lua(sandbox, L, status);
		lua_settop(L, top);
		return false;
	}

	lua_remove(L, handler);

	return true;
}

/* Argument 1 is the path descriptor, argument 2 the value to store. */
static int luaext_exec_path_writer(lua_State *L)
{
	const luaext_exec_path *request = (const luaext_exec_path *)lua_touserdata(L, 1);
	size_t cursor = 0;

	lua_settop(L, 2);
	lua_pushglobaltable(L);

	for (;;) {
		const char *component = request->path + cursor;
		const char *dot = (const char *)memchr(component, '.', request->len - cursor);
		size_t component_len = dot != NULL ? (size_t)(dot - component) : request->len - cursor;

		if (dot == NULL) {
			lua_pushlstring(L, component, component_len);
			lua_pushvalue(L, 2);

			/*
			 * Raw, so a table carrying __newindex cannot intercept a host
			 * assignment -- and so that storing nil deletes the key, which is
			 * the only way to express "unset" from PHP.
			 */
			lua_rawset(L, -3);
			break;
		}

		lua_pushlstring(L, component, component_len);
		lua_rawget(L, -2);

		if (lua_isnil(L, -1)) {
			/* Intermediates are created rather than refused: setGlobal("a.b", 1)
			 * on a fresh sandbox is a reasonable thing for a host to write. */
			lua_pop(L, 1);
			lua_createtable(L, 0, 1);
			lua_pushlstring(L, component, component_len);
			lua_pushvalue(L, -2);
			lua_rawset(L, -4);
		} else if (!lua_istable(L, -1)) {
			/* `cursor` still points at the start of this component, so the
			 * prefix that names the offending value runs through the end of it
			 * -- not up to it, the way it does on the read side. */
			luaext_exec_path_refuse(L, request, cursor + component_len);
		}

		lua_remove(L, -2);
		cursor += component_len + 1;
	}

	return 0;
}

bool luaext_exec_assign_path(luaext_sandbox *sandbox, const char *path, size_t path_len)
{
	luaext_exec_path request;
	lua_State *L;
	int value_index;
	int base;
	int handler;
	int status;

	if (!luaext_exec_ready(sandbox)) {
		return false;
	}

	L = luaext_exec_state(sandbox);
	value_index = lua_gettop(L);
	base = value_index - 1;

	/* Popped on every path, including this one: the caller pushed a value and
	 * is entitled to a balanced stack whatever the answer turns out to be. */
	if (!luaext_exec_path_valid(path, path_len)) {
		lua_settop(L, base);
		return false;
	}

	if (!lua_checkstack(L, 8)) {
		lua_settop(L, base);
		zend_throw_exception(luaext_ce_runtime_error,
							 "Cannot write a Lua global: the interpreter stack cannot grow", 0);
		return false;
	}

	request.path = path;
	request.len = path_len;

	lua_pushcfunction(L, luaext_error_traceback_handler);
	handler = lua_gettop(L);

	lua_pushcfunction(L, luaext_exec_path_writer);
	lua_pushlightuserdata(L, &request);
	lua_pushvalue(L, value_index);

	status = lua_pcall(L, 2, 0, handler);

	if (status != LUA_OK) {
		luaext_error_throw_from_lua(sandbox, L, status);
		lua_settop(L, base);
		return false;
	}

	lua_settop(L, base);

	return true;
}

/* -------------------------------------------------------------------------
 * Function handles
 * ---------------------------------------------------------------------- */

void luaext_exec_make_function(luaext_sandbox *sandbox, zval *sandbox_zv, zval *return_value)
{
	luaext_function_obj *function;
	lua_State *L;
	int ref;

	if (!luaext_exec_ready(sandbox)) {
		return;
	}

	L = luaext_exec_state(sandbox);

	if (!lua_isfunction(L, -1)) {
		zend_throw_exception_ex(luaext_ce_conversion_error, 0,
								"Cannot build a LuaFunction from a %s", luaL_typename(L, -1));
		lua_pop(L, 1);
		return;
	}

	/*
	 * The slot is taken before the PHP object exists. Reserving it runs an
	 * allocation inside the interpreter, and doing that while holding a
	 * half-built zend_object would leave the object owning a slot it never
	 * learned the number of.
	 */
	ref = luaext_convert_ref_create(sandbox, L, -1);
	lua_pop(L, 1);

	/* Slots are numbered from one; zero belongs to the freelist's own owner. */
	if (ref <= 0) {
		return;
	}

	object_init_ex(return_value, luaext_ce_lua_function);

	function = Z_LUAEXT_FUNCTION_P(return_value);
	function->ref = ref;

	/*
	 * The handle keeps its sandbox alive: the slot it names lives in that
	 * sandbox's registry, and the freelist it is returned to lives in the
	 * sandbox struct, so the handle must not be able to outlive either.
	 */
	ZVAL_COPY(&function->sandbox_zv, sandbox_zv);
}
