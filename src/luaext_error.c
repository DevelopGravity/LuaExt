/*
 * luaext — errors across the PHP/Lua boundary.
 *
 * One rule shapes everything here: a script may catch its own mistakes and
 * nothing else. If pcall could swallow a CPU-limit or memory-limit error, every
 * limit in this extension would be advisory, so the value that carries a fatal
 * error has to be one Lua code can neither forge nor disarm.
 *
 * That value is a full userdata. Lua has no way to construct userdata, and the
 * metatable that identifies ours is stored only in the registry and sets
 * __metatable, so getmetatable() cannot reach it and setmetatable() cannot
 * copy it onto a value a script made itself. Recognising a fatal error is then
 * a pointer comparison against the registry's copy, not a convention a script
 * could imitate.
 *
 * The second rule is that nothing degrades on the way out. A PHP callback that
 * throws has its exception object retained, not its message text, so the host
 * catches the class it threw rather than a string that used to be one.
 */

#include "luaext_error.h"

#include <lauxlib.h>
#include <lua.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <Zend/zend_exceptions.h>
#include <Zend/zend_object_handlers.h>
#include <Zend/zend_operators.h>

/*
 * Raise sites format into this rather than onto the heap. lua_error() longjmps,
 * so a heap buffer would need to be freed before the raise and could not be
 * freed if building the error value itself failed; a stack buffer removes the
 * window entirely.
 */
#define LUAEXT_ERROR_MESSAGE_MAX 512

/*
 * Innermost frames kept in a traceback.
 *
 * Runaway recursion stacks thousands of near-identical frames and the host has
 * to materialise every captured one as a PHP array. The innermost handful are
 * the only ones that ever explain anything, so the tail is dropped rather than
 * turned into a second denial of service on the way out of the first.
 */
#define LUAEXT_ERROR_TRACE_FRAMES 64

/*
 * Where the Lua context lives on a thrown exception.
 *
 * The names are mangled the way the engine mangles a private property
 * ("\0<class>\0<name>") naming a class that does not exist, so PHP has no
 * syntax that reads or writes them. A host cannot forge a Lua traceback onto an
 * exception it constructed itself, and cannot strip one off an exception the
 * sandbox threw. The stub declares no properties, so this is also the only way
 * to carry the context without editing a shared file.
 */
#define LUAEXT_KEY_TRACE "\0luaext\0luaTrace"
#define LUAEXT_KEY_TRACE_LEN (sizeof(LUAEXT_KEY_TRACE) - 1)
#define LUAEXT_KEY_SANDBOX "\0luaext\0sandbox"
#define LUAEXT_KEY_SANDBOX_LEN (sizeof(LUAEXT_KEY_SANDBOX) - 1)

static int luaext_error_capture(lua_State *L);

/* -------------------------------------------------------------------------
 * The error metatable
 * ---------------------------------------------------------------------- */

/*
 * Release what the userdata owns outside the Lua heap.
 *
 * Runs from the collector and from lua_close(), including the request-shutdown
 * sweep, which is why the message is allocated persistently: a request-arena
 * string could already have been reclaimed by the time this runs.
 */
static int luaext_error_gc(lua_State *L)
{
	luaext_error_ud *error = (luaext_error_ud *)lua_touserdata(L, 1);

	if (error == NULL || error->magic != LUAEXT_ERROR_MAGIC) {
		return 0;
	}

	/* A finalised error is no longer one of ours, even if it is resurrected. */
	error->magic = 0;

	if (error->message != NULL) {
		zend_string_release(error->message);
		error->message = NULL;
	}

	if (Z_TYPE(error->php_exception) == IS_OBJECT) {
		zval_ptr_dtor(&error->php_exception);
	}

	ZVAL_UNDEF(&error->php_exception);

	return 0;
}

/*
 * Give naive handlers something sensible. A script that does
 * `print(select(2, pcall(f)))` should see the message, not "userdata: 0x…" —
 * which would also hand it a heap address the rest of the sandbox works to keep
 * hidden.
 */
static int luaext_error_tostring(lua_State *L)
{
	const luaext_error_ud *error = (const luaext_error_ud *)lua_touserdata(L, 1);

	if (error == NULL || error->magic != LUAEXT_ERROR_MAGIC || error->message == NULL) {
		lua_pushliteral(L, "luaext error");
		return 1;
	}

	lua_pushlstring(L, ZSTR_VAL(error->message), ZSTR_LEN(error->message));

	return 1;
}

void luaext_error_init(luaext_sandbox *sandbox)
{
	lua_State *L;

	if (sandbox == NULL || sandbox->L == NULL) {
		return;
	}

	L = sandbox->L;

	if (!lua_checkstack(L, 4)) {
		ZEND_ASSERT(0 && "luaext: no stack to install the error metatable");
		return;
	}

	lua_rawgetp(L, LUA_REGISTRYINDEX, &luaext_key_errmt);

	if (lua_type(L, -1) == LUA_TTABLE) {
		lua_pop(L, 1);
		return;
	}

	lua_pop(L, 1);
	lua_createtable(L, 0, 3);

	lua_pushcfunction(L, luaext_error_tostring);
	lua_setfield(L, -2, "__tostring");

	lua_pushcfunction(L, luaext_error_gc);
	lua_setfield(L, -2, "__gc");

	/*
	 * getmetatable(err) yields false instead of the table, and setmetatable()
	 * on one of these userdata raises. Between them a script can neither read
	 * __gc/__tostring back out, nor replace them, nor stamp this metatable onto
	 * a value of its own to make a forgery pass the identity check below.
	 */
	lua_pushboolean(L, 0);
	lua_setfield(L, -2, "__metatable");

	lua_rawsetp(L, LUA_REGISTRYINDEX, &luaext_key_errmt);
}

/* -------------------------------------------------------------------------
 * Building error values
 * ---------------------------------------------------------------------- */

/*
 * Push an error userdata carrying `message`.
 *
 * Returns NULL, having pushed the message as a plain Lua string instead, if
 * this state never received its metatable. The caller still has a raisable
 * value, but there is nothing to hang a retained exception or a traceback on —
 * and the error is catchable, so luaext_error_init() failing is a real loss of
 * guarantee rather than a cosmetic one.
 */
static luaext_error_ud *luaext_error_push(lua_State *L, luaext_err_kind kind, bool fatal,
										  const char *message, size_t message_len)
{
	luaext_error_ud *error;

	/*
	 * Lua guarantees a C function LUA_MINSTACK free slots and keeps a further
	 * reserve for error handling, so this only fails when the stack cannot grow
	 * at all — at which point nothing can be described.
	 */
	if (!lua_checkstack(L, 4)) {
		ZEND_ASSERT(0 && "luaext: no stack to build an error value");
		lua_pushlstring(L, message, message_len);
		return NULL;
	}

	lua_rawgetp(L, LUA_REGISTRYINDEX, &luaext_key_errmt);

	if (lua_type(L, -1) != LUA_TTABLE) {
		lua_pop(L, 1);
		lua_pushlstring(L, message, message_len);
		return NULL;
	}

	error = (luaext_error_ud *)lua_newuserdatauv(L, sizeof(*error), 1);

	memset(error, 0, sizeof(*error));
	error->magic = LUAEXT_ERROR_MAGIC;
	error->kind = (uint8_t)kind;
	error->fatal = fatal;
	ZVAL_UNDEF(&error->php_exception);

	/* Arm __gc before anything the finaliser must release is stored. */
	lua_pushvalue(L, -2);
	lua_setmetatable(L, -2);

	/*
	 * Persistent rather than request-allocated: __gc can run from lua_close()
	 * during request shutdown, and a string from the request arena would be
	 * released after the arena had gone.
	 */
	error->message = zend_string_init(message, message_len, 1);

	lua_remove(L, -2);

	return error;
}

/* -------------------------------------------------------------------------
 * Raising
 * ---------------------------------------------------------------------- */

#ifdef LUAEXT_DEBUG
/*
 * lua_error() unwinds the C stack without running any cleanup. A frame that
 * still owns a zval or a zend_string therefore leaks it, silently, on a path
 * that only executes when something has already gone wrong. Debug builds refuse
 * to let that ship: bracket such a region with LUAEXT_NO_RAISE_BEGIN/END and
 * this fires the moment a raise escapes it.
 */
static void luaext_error_assert_may_raise(lua_State *L)
{
	const luaext_sandbox *sandbox = LUAEXT_SB(L);

	ZEND_ASSERT((sandbox == NULL || sandbox->no_raise_depth == 0) &&
				"luaext: raising a Lua error while a frame still owns a resource");
}
#else
#define luaext_error_assert_may_raise(L) ((void)(L))
#endif

ZEND_COLD ZEND_NORETURN void luaext_error_raise(lua_State *L, luaext_err_kind kind, bool fatal,
												const char *format, ...)
{
	char message[LUAEXT_ERROR_MESSAGE_MAX];
	va_list arguments;
	int length;

	luaext_error_assert_may_raise(L);

	va_start(arguments, format);
	length = vsnprintf(message, sizeof(message), format, arguments);
	va_end(arguments);

	if (length < 0) {
		length = 0;
	} else if ((size_t)length >= sizeof(message)) {
		length = (int)sizeof(message) - 1;
	}

	luaext_error_push(L, kind, fatal, message, (size_t)length);

	lua_error(L);
	ZEND_UNREACHABLE();
}

/*
 * Read a throwable's message without caring which of the two engine roots
 * declares it. `message` is protected, so the scope has to be a class that can
 * see it, and Error and Exception declare their own.
 */
static zend_string *luaext_error_exception_message(zend_object *exception)
{
	zend_class_entry *scope =
		instanceof_function(exception->ce, zend_ce_error) ? zend_ce_error : zend_ce_exception;
	zval holder;
	zval *message =
		zend_read_property_ex(scope, exception, ZSTR_KNOWN(ZEND_STR_MESSAGE), true, &holder);

	if (message == NULL || Z_TYPE_P(message) != IS_STRING) {
		return NULL;
	}

	/* Borrowed: the exception owns it and outlives this call. */
	return Z_STR_P(message);
}

ZEND_COLD ZEND_NORETURN void luaext_error_raise_from_exception(lua_State *L)
{
	zend_object *exception = EG(exception);
	zend_string *message;
	luaext_error_ud *error;
	bool fatal;

	luaext_error_assert_may_raise(L);

	if (exception == NULL) {
		luaext_error_raise(L, LUAEXT_ERR_RUNTIME, false, "%s",
						   "a host callback failed without reporting an exception");
	}

	/*
	 * The contract the whole PHP-callback boundary rests on. A RuntimeError is
	 * the host saying "the script is meant to handle this", so it stays
	 * catchable. Anything else — a TypeError in the callback, a database driver
	 * failing, an engine Error — is not the script's to swallow.
	 */
	fatal = !instanceof_function(exception->ce, luaext_ce_runtime_error);
	message = luaext_error_exception_message(exception);

	error = luaext_error_push(L, fatal ? LUAEXT_ERR_ABORT : LUAEXT_ERR_RUNTIME, fatal,
							  message != NULL ? ZSTR_VAL(message) : "",
							  message != NULL ? ZSTR_LEN(message) : 0);

	if (error != NULL) {
		/*
		 * Retain the object, not its text: this is the whole reason the host
		 * gets its own exception class back on the other side rather than a
		 * string that used to be one.
		 */
		ZVAL_OBJ(&error->php_exception, exception);
		GC_ADDREF(exception);

		/* From here the error travels as a Lua value. */
		zend_clear_exception();
	}

	/*
	 * Otherwise the exception deliberately stays pending: with no value to
	 * retain it on, leaving it in flight at least chains it onto whatever this
	 * unwind eventually throws instead of discarding it.
	 */

	lua_error(L);
	ZEND_UNREACHABLE();
}

/* -------------------------------------------------------------------------
 * Identifying our errors
 * ---------------------------------------------------------------------- */

bool luaext_error_is_ours(lua_State *L, int index)
{
	const luaext_error_ud *error;
	bool ours;

	/* Full userdata only: a light userdata is a bare pointer with no metatable. */
	if (lua_type(L, index) != LUA_TUSERDATA) {
		return false;
	}

	/*
	 * Checked before the magic word is read, so that a smaller userdata
	 * belonging to some other extension is never read past its end.
	 */
	if (lua_rawlen(L, index) != sizeof(luaext_error_ud)) {
		return false;
	}

	error = (const luaext_error_ud *)lua_touserdata(L, index);

	if (error == NULL || error->magic != LUAEXT_ERROR_MAGIC) {
		return false;
	}

	if (!lua_checkstack(L, 2)) {
		return false;
	}

	index = lua_absindex(L, index);

	if (!lua_getmetatable(L, index)) {
		return false;
	}

	/*
	 * The proof. The magic word only says the memory looks right; identity with
	 * the registry's own table is what a script cannot reproduce, because it
	 * can neither read the table out of a userdata nor write it onto one.
	 */
	lua_rawgetp(L, LUA_REGISTRYINDEX, &luaext_key_errmt);
	ours = lua_rawequal(L, -1, -2) != 0;
	lua_pop(L, 2);

	return ours;
}

bool luaext_error_is_fatal(lua_State *L, int index)
{
	if (!luaext_error_is_ours(L, index)) {
		return false;
	}

	return ((const luaext_error_ud *)lua_touserdata(L, index))->fatal;
}

/* -------------------------------------------------------------------------
 * Capturing a traceback
 * ---------------------------------------------------------------------- */

/*
 * True while walking the frames this subsystem itself put on the stack.
 *
 * Found rather than counted: the handler runs the capture inside a nested
 * protected call, and hard-coding how many frames that adds would be a silent
 * off-by-one the day the arrangement changes.
 */
static bool luaext_error_is_own_frame(lua_State *L, lua_Debug *frame)
{
	lua_CFunction function;

	lua_getinfo(L, "f", frame);
	function = lua_tocfunction(L, -1);
	lua_pop(L, 1);

	return function == luaext_error_capture || function == luaext_error_traceback_handler;
}

/*
 * Record the erroring stack on the error value, innermost frame first, in the
 * shape LuaThrowable::getLuaTrace() documents.
 *
 * Kept as a Lua table in uservalue 1 rather than as C memory: it is then plain
 * garbage-collected data with no ownership to get wrong on an unwind path.
 */
static void luaext_error_attach_trace(lua_State *L, int error_index)
{
	lua_Debug frame;
	int level = 0;
	int captured = 0;

	error_index = lua_absindex(L, error_index);

	/*
	 * An error re-raised across a coroutine boundary passes here twice; the
	 * first traceback is the one taken where the failure happened.
	 */
	if (lua_getiuservalue(L, error_index, 1) != LUA_TNIL) {
		lua_pop(L, 1);
		return;
	}

	lua_pop(L, 1);

	while (lua_getstack(L, level, &frame) && luaext_error_is_own_frame(L, &frame)) {
		level++;
	}

	lua_newtable(L);

	while (captured < LUAEXT_ERROR_TRACE_FRAMES && lua_getstack(L, level, &frame)) {
		lua_getinfo(L, "Slnt", &frame);

		lua_createtable(L, 0, 6);

		/*
		 * short_src, not source: the raw source of a chunk loaded from a string
		 * is the script text itself, and a traceback is not the place to hand
		 * the host a megabyte of it.
		 */
		lua_pushstring(L, frame.short_src);
		lua_setfield(L, -2, "source");

		lua_pushstring(L, frame.what != NULL ? frame.what : "");
		lua_setfield(L, -2, "what");

		lua_pushinteger(L, frame.currentline);
		lua_setfield(L, -2, "currentLine");

		if (frame.name != NULL) {
			lua_pushstring(L, frame.name);
		} else {
			lua_pushnil(L);
		}

		lua_setfield(L, -2, "name");

		lua_pushstring(L, frame.namewhat != NULL ? frame.namewhat : "");
		lua_setfield(L, -2, "nameWhat");

		lua_pushinteger(L, frame.linedefined);
		lua_setfield(L, -2, "lineDefined");

		lua_rawseti(L, -2, ++captured);
		level++;
	}

	lua_setiuservalue(L, error_index, 1);
}

/*
 * The part of the message handler that is allowed to fail. Runs inside a nested
 * protected call so that failing costs a traceback and nothing else.
 */
static int luaext_error_capture(lua_State *L)
{
	luaL_checkstack(L, 8, "luaext: no stack for a traceback");

	if (luaext_error_is_ours(L, 1)) {
		lua_pushvalue(L, 1);
	} else {
		size_t length = 0;
		const char *message;

		/*
		 * Nothing assumes a string. Lua 5.5 substitutes one for a nil error
		 * object, but a script can still raise a table, a number, or a value
		 * whose __tostring misbehaves — which is exactly why this runs
		 * protected.
		 */
		message = luaL_tolstring(L, 1, &length);

		if (luaext_error_push(L, LUAEXT_ERR_RUNTIME, false, message, length) == NULL) {
			return 1;
		}

		lua_remove(L, -2);
	}

	luaext_error_attach_trace(L, -1);

	return 1;
}

int luaext_error_traceback_handler(lua_State *L)
{
	luaext_sandbox *sandbox = LUAEXT_SB(L);
	size_t saved_limit = 0;
	bool lifted = false;

	if (lua_gettop(L) == 0) {
		lua_pushnil(L);
	}

	/*
	 * Reporting that a script ran out of memory must not itself fail for want
	 * of memory, so the ceiling comes off for the duration.
	 *
	 * The allocator subsystem owns luaext_alloc.c; this deliberately writes the
	 * field rather than calling luaext_alloc_set_limit(), because that call is
	 * also where GC re-tuning lives and re-tuning the collector in the middle of
	 * an unwind is not something this path should be doing.
	 */
	if (sandbox != NULL) {
		saved_limit = sandbox->alloc.limit;
		sandbox->alloc.limit = 0;
		lifted = true;
	}

	/*
	 * Everything that can fail runs inside the nested protected call, so the
	 * ceiling is always restored. lua_pcall catches even an error raised while
	 * handling this error, and every operation outside it — gettop, pushing nil,
	 * checkstack, pushing a light C function, pushvalue, replace, settop —
	 * allocates nothing and cannot raise.
	 */
	if (lua_checkstack(L, 4)) {
		lua_pushcfunction(L, luaext_error_capture);
		lua_pushvalue(L, 1);

		if (lua_pcall(L, 1, 1, 0) == LUA_OK) {
			lua_replace(L, 1);
		} else {
			/* A secondary failure is noise; the original error is the answer. */
			lua_pop(L, 1);
		}
	}

	if (lifted) {
		sandbox->alloc.limit = saved_limit;
	}

	lua_settop(L, 1);

	return 1;
}

/* -------------------------------------------------------------------------
 * Surfacing the traceback to PHP
 * ---------------------------------------------------------------------- */

static void luaext_error_push_field(lua_State *L, int frame_index, const char *field)
{
	lua_pushstring(L, field);
	lua_rawget(L, frame_index);
}

static void luaext_error_copy_string_field(lua_State *L, int frame_index, const char *field,
										   zval *target, bool nullable)
{
	luaext_error_push_field(L, frame_index, field);

	if (lua_type(L, -1) == LUA_TSTRING) {
		size_t length = 0;
		const char *value = lua_tolstring(L, -1, &length);

		add_assoc_stringl(target, field, value, length);
	} else if (nullable) {
		add_assoc_null(target, field);
	} else {
		add_assoc_string(target, field, "");
	}

	lua_pop(L, 1);
}

static void luaext_error_copy_long_field(lua_State *L, int frame_index, const char *field,
										 zval *target)
{
	luaext_error_push_field(L, frame_index, field);
	add_assoc_long(target, field, (zend_long)lua_tointeger(L, -1));
	lua_pop(L, 1);
}

void luaext_error_trace_to_zval(lua_State *L, int index, zval *out)
{
	int position;

	ZVAL_NULL(out);

	if (!luaext_error_is_ours(L, index) || !lua_checkstack(L, 4)) {
		return;
	}

	index = lua_absindex(L, index);

	if (lua_getiuservalue(L, index, 1) != LUA_TTABLE) {
		lua_pop(L, 1);
		return;
	}

	array_init(out);

	for (position = 1;; position++) {
		zval php_frame;
		int frame_index;

		if (lua_rawgeti(L, -1, position) != LUA_TTABLE) {
			lua_pop(L, 1);
			break;
		}

		frame_index = lua_gettop(L);

		array_init_size(&php_frame, 6);
		luaext_error_copy_string_field(L, frame_index, "source", &php_frame, false);
		luaext_error_copy_string_field(L, frame_index, "what", &php_frame, false);
		luaext_error_copy_long_field(L, frame_index, "currentLine", &php_frame);
		luaext_error_copy_string_field(L, frame_index, "name", &php_frame, true);
		luaext_error_copy_string_field(L, frame_index, "nameWhat", &php_frame, false);
		luaext_error_copy_long_field(L, frame_index, "lineDefined", &php_frame);

		add_next_index_zval(out, &php_frame);
		lua_pop(L, 1);
	}

	lua_pop(L, 1);
}

/* -------------------------------------------------------------------------
 * Throwing into PHP
 * ---------------------------------------------------------------------- */

static void luaext_error_store(zend_object *exception, const char *key, size_t key_len, zval *value)
{
	zend_hash_str_update(zend_std_get_properties_ex(exception), key, key_len, value);
}

static zval *luaext_error_fetch(zend_object *exception, const char *key, size_t key_len)
{
	/* Never builds the table just to read it: an exception the sandbox did not
	 * throw has no properties table at all, and should not gain one. */
	if (exception->properties == NULL) {
		return NULL;
	}

	return zend_hash_str_find(exception->properties, key, key_len);
}

/*
 * Attach the Lua context to a thrown exception. Consumes `trace`.
 */
static void luaext_error_attach(zend_object *exception, luaext_sandbox *sandbox, zval *trace)
{
	if (!instanceof_function(exception->ce, luaext_ce_lua_throwable)) {
		/*
		 * A host exception of an unrelated class is rethrown exactly as it was.
		 * It has no accessor that could report a Lua traceback, and hanging one
		 * off it would show up nowhere but var_dump.
		 */
		zval_ptr_dtor(trace);
		ZVAL_UNDEF(trace);
		return;
	}

	luaext_error_store(exception, LUAEXT_KEY_TRACE, LUAEXT_KEY_TRACE_LEN, trace);
	ZVAL_UNDEF(trace);

	if (sandbox != NULL && !sandbox->closed) {
		zval owner;

		ZVAL_OBJ_COPY(&owner, &sandbox->std);
		luaext_error_store(exception, LUAEXT_KEY_SANDBOX, LUAEXT_KEY_SANDBOX_LEN, &owner);
	}
}

static zend_class_entry *luaext_error_class_for(const luaext_error_ud *error, int status)
{
	zend_class_entry *ce;

	if (error == NULL) {
		/*
		 * Errors Lua raised itself, where the status is the only classification
		 * available: the value is a bare string the interpreter produced.
		 */
		switch (status) {
		case LUA_ERRSYNTAX:
			return luaext_ce_syntax_error;
		case LUA_ERRMEM:
			return luaext_ce_memory_limit_error;
		case LUA_ERRERR:
			return luaext_ce_error_handler_error;
		default:
			return luaext_ce_runtime_error;
		}
	}

	switch ((luaext_err_kind)error->kind) {
	case LUAEXT_ERR_SYNTAX:
		ce = luaext_ce_syntax_error;
		break;
	case LUAEXT_ERR_MEMORY:
		ce = luaext_ce_memory_limit_error;
		break;
	case LUAEXT_ERR_CPU:
		ce = luaext_ce_cpu_limit_error;
		break;
	case LUAEXT_ERR_WALL:
		ce = luaext_ce_wall_clock_limit_error;
		break;
	case LUAEXT_ERR_OUTPUT:
		ce = luaext_ce_output_limit_error;
		break;
	case LUAEXT_ERR_COROUTINE:
		ce = luaext_ce_coroutine_limit_error;
		break;
	case LUAEXT_ERR_ABORT:
		ce = luaext_ce_host_abort_error;
		break;
	case LUAEXT_ERR_HANDLER:
		ce = luaext_ce_error_handler_error;
		break;
	case LUAEXT_ERR_PANIC:
		ce = luaext_ce_panic_error;
		break;
	case LUAEXT_ERR_CONVERSION:
		ce = luaext_ce_conversion_error;
		break;
	case LUAEXT_ERR_VFS:
		ce = luaext_ce_vfs_error;
		break;
	case LUAEXT_ERR_MODULE:
		ce = luaext_ce_module_not_found_error;
		break;
	case LUAEXT_ERR_RUNTIME:
	default:
		ce = luaext_ce_runtime_error;
		break;
	}

	/*
	 * The fatal flag wins over the kind. A fatal error surfacing as a class the
	 * hierarchy calls catchable would be a lie to the host and, worse, an
	 * invitation for a future pcall replacement to decide it was catchable
	 * after all.
	 */
	if (error->fatal && !instanceof_function(ce, luaext_ce_fatal_error)) {
		ce = luaext_ce_host_abort_error;
	}

	return ce;
}

static zend_string *luaext_error_message_for(lua_State *L, int index, const luaext_error_ud *error,
											 int status)
{
	if (error != NULL && error->message != NULL) {
		return zend_string_init(ZSTR_VAL(error->message), ZSTR_LEN(error->message), 0);
	}

	/*
	 * Only a genuine string is read back. Converting a number would make Lua
	 * allocate, and LUA_ERRMEM is precisely the case where it cannot — which is
	 * why this path exists at all rather than calling luaL_tolstring.
	 */
	if (lua_type(L, index) == LUA_TSTRING) {
		size_t length = 0;
		const char *value = lua_tolstring(L, index, &length);

		return zend_string_init(value, length, 0);
	}

	switch (status) {
	case LUA_ERRMEM:
		return zend_string_init(ZEND_STRL("The script exhausted its memory budget"), 0);
	case LUA_ERRERR:
		return zend_string_init(ZEND_STRL("A Lua error handler failed while handling an error"), 0);
	default:
		break;
	}

	return zend_strpprintf(0, "Lua raised a %s error value", lua_typename(L, lua_type(L, index)));
}

void luaext_error_throw_from_lua(luaext_sandbox *sandbox, lua_State *L, int status)
{
	const luaext_error_ud *error = NULL;
	zend_object *thrown;
	zval trace;

	if (L == NULL) {
		return;
	}

	if (lua_gettop(L) == 0) {
		zend_throw_exception(luaext_error_class_for(NULL, status),
							 "The Lua interpreter failed without producing an error value", 0);
		return;
	}

	if (luaext_error_is_ours(L, -1)) {
		error = (const luaext_error_ud *)lua_touserdata(L, -1);
	}

	luaext_error_trace_to_zval(L, -1, &trace);

	if (error != NULL && Z_TYPE(error->php_exception) == IS_OBJECT) {
		zval rethrown;

		/*
		 * The host gets back the object it threw: same class, same code, same
		 * properties. zend_throw_exception_object() chains whatever was already
		 * in flight as the previous exception and takes this reference.
		 */
		ZVAL_COPY(&rethrown, &error->php_exception);
		thrown = Z_OBJ(rethrown);

		luaext_error_attach(thrown, sandbox, &trace);
		zend_throw_exception_object(&rethrown);
	} else {
		zend_string *message = luaext_error_message_for(L, -1, error, status);

		thrown = zend_throw_exception(luaext_error_class_for(error, status), ZSTR_VAL(message), 0);
		zend_string_release(message);

		if (thrown != NULL) {
			luaext_error_attach(thrown, sandbox, &trace);
		} else {
			zval_ptr_dtor(&trace);
		}
	}

	lua_pop(L, 1);
}

/* -------------------------------------------------------------------------
 * LuaThrowable accessors
 *
 * Shared by both exception roots: a failure carries the same Lua context
 * whether it was the script's fault or the host's.
 * ---------------------------------------------------------------------- */

static const char *luaext_frame_string(const HashTable *frame, const char *key, size_t key_len,
									   const char *fallback)
{
	const zval *value = zend_hash_str_find(frame, key, key_len);

	if (value == NULL || Z_TYPE_P(value) != IS_STRING) {
		return fallback;
	}

	return Z_STRVAL_P(value);
}

static zend_long luaext_frame_long(const HashTable *frame, const char *key, size_t key_len)
{
	const zval *value = zend_hash_str_find(frame, key, key_len);

	if (value == NULL || Z_TYPE_P(value) != IS_LONG) {
		return 0;
	}

	return Z_LVAL_P(value);
}

static zval *luaext_this_trace(zval *this_zv)
{
	zval *trace = luaext_error_fetch(Z_OBJ_P(this_zv), LUAEXT_KEY_TRACE, LUAEXT_KEY_TRACE_LEN);

	if (trace == NULL || Z_TYPE_P(trace) != IS_ARRAY) {
		return NULL;
	}

	return trace;
}

/* The innermost frame that is not a C function, which is where a Lua-level
 * failure is reported from. */
static const HashTable *luaext_this_lua_frame(zval *this_zv)
{
	zval *trace = luaext_this_trace(this_zv);
	zval *frame;

	if (trace == NULL) {
		return NULL;
	}

	ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(trace), frame)
	{
		if (Z_TYPE_P(frame) != IS_ARRAY) {
			continue;
		}

		if (strcmp(luaext_frame_string(Z_ARRVAL_P(frame), ZEND_STRL("what"), "C"), "C") != 0) {
			return Z_ARRVAL_P(frame);
		}
	}
	ZEND_HASH_FOREACH_END();

	return NULL;
}

/* One traceback line, in the interpreter's own layout. */
static void luaext_append_frame(smart_str *buffer, const HashTable *frame)
{
	const char *source = luaext_frame_string(frame, ZEND_STRL("source"), "?");
	const char *what = luaext_frame_string(frame, ZEND_STRL("what"), "");
	const char *name = luaext_frame_string(frame, ZEND_STRL("name"), NULL);
	const char *name_what = luaext_frame_string(frame, ZEND_STRL("nameWhat"), "");
	zend_long current_line = luaext_frame_long(frame, ZEND_STRL("currentLine"));

	smart_str_appends(buffer, "\n\t");
	smart_str_appends(buffer, source);
	smart_str_appendc(buffer, ':');

	if (current_line > 0) {
		smart_str_append_long(buffer, current_line);
		smart_str_appendc(buffer, ':');
	}

	smart_str_appends(buffer, " in ");

	if (name != NULL && name_what[0] != '\0') {
		smart_str_appends(buffer, name_what);
		smart_str_appends(buffer, " '");
		smart_str_appends(buffer, name);
		smart_str_appendc(buffer, '\'');
	} else if (strcmp(what, "main") == 0) {
		smart_str_appends(buffer, "main chunk");
	} else if (strcmp(what, "C") != 0) {
		smart_str_appends(buffer, "function <");
		smart_str_appends(buffer, source);
		smart_str_appendc(buffer, ':');
		smart_str_append_long(buffer, luaext_frame_long(frame, ZEND_STRL("lineDefined")));
		smart_str_appendc(buffer, '>');
	} else {
		smart_str_appendc(buffer, '?');
	}
}

static void luaext_return_trace(zval *this_zv, zval *return_value)
{
	zval *trace = luaext_this_trace(this_zv);

	if (trace == NULL) {
		RETURN_NULL();
	}

	RETURN_COPY(trace);
}

static void luaext_return_trace_string(zval *this_zv, zval *return_value)
{
	zval *trace = luaext_this_trace(this_zv);
	smart_str buffer = {0};
	zval *frame;

	if (trace == NULL || zend_hash_num_elements(Z_ARRVAL_P(trace)) == 0) {
		RETURN_EMPTY_STRING();
	}

	smart_str_appends(&buffer, "stack traceback:");

	ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(trace), frame)
	{
		if (Z_TYPE_P(frame) == IS_ARRAY) {
			luaext_append_frame(&buffer, Z_ARRVAL_P(frame));
		}
	}
	ZEND_HASH_FOREACH_END();

	RETURN_STR(smart_str_extract(&buffer));
}

static void luaext_return_sandbox(zval *this_zv, zval *return_value)
{
	zval *owner = luaext_error_fetch(Z_OBJ_P(this_zv), LUAEXT_KEY_SANDBOX, LUAEXT_KEY_SANDBOX_LEN);

	if (owner == NULL || Z_TYPE_P(owner) != IS_OBJECT ||
		!instanceof_function(Z_OBJCE_P(owner), luaext_ce_sandbox)) {
		RETURN_NULL();
	}

	/* Documented as null once the sandbox is closed: the handle is still here,
	 * but there is no interpreter behind it to ask anything of. */
	if (Z_LUAEXT_SANDBOX_P(owner)->closed) {
		RETURN_NULL();
	}

	RETURN_COPY(owner);
}

static void luaext_return_chunk_name(zval *this_zv, zval *return_value)
{
	const HashTable *frame = luaext_this_lua_frame(this_zv);
	const zval *source;

	if (frame == NULL) {
		RETURN_NULL();
	}

	source = zend_hash_str_find(frame, ZEND_STRL("source"));

	if (source == NULL || Z_TYPE_P(source) != IS_STRING) {
		RETURN_NULL();
	}

	RETURN_STR_COPY(Z_STR_P(source));
}

static void luaext_return_lua_line(zval *this_zv, zval *return_value)
{
	const HashTable *frame = luaext_this_lua_frame(this_zv);
	zend_long line;

	if (frame == NULL) {
		RETURN_NULL();
	}

	line = luaext_frame_long(frame, ZEND_STRL("currentLine"));

	if (line <= 0) {
		RETURN_NULL();
	}

	RETURN_LONG(line);
}

#define LUAEXT_DEFINE_TRACE_ACCESSORS(base_class)                                                  \
	ZEND_METHOD(base_class, getLuaTrace)                                                           \
	{                                                                                              \
		ZEND_PARSE_PARAMETERS_NONE();                                                              \
		luaext_return_trace(ZEND_THIS, return_value);                                              \
	}                                                                                              \
                                                                                                   \
	ZEND_METHOD(base_class, getLuaTraceAsString)                                                   \
	{                                                                                              \
		ZEND_PARSE_PARAMETERS_NONE();                                                              \
		luaext_return_trace_string(ZEND_THIS, return_value);                                       \
	}                                                                                              \
                                                                                                   \
	ZEND_METHOD(base_class, getSandbox)                                                            \
	{                                                                                              \
		ZEND_PARSE_PARAMETERS_NONE();                                                              \
		luaext_return_sandbox(ZEND_THIS, return_value);                                            \
	}                                                                                              \
                                                                                                   \
	ZEND_METHOD(base_class, getChunkName)                                                          \
	{                                                                                              \
		ZEND_PARSE_PARAMETERS_NONE();                                                              \
		luaext_return_chunk_name(ZEND_THIS, return_value);                                         \
	}                                                                                              \
                                                                                                   \
	ZEND_METHOD(base_class, getLuaLine)                                                            \
	{                                                                                              \
		ZEND_PARSE_PARAMETERS_NONE();                                                              \
		luaext_return_lua_line(ZEND_THIS, return_value);                                           \
	}

LUAEXT_DEFINE_TRACE_ACCESSORS(DevelopGravity_LuaExt_Exception_LuaException)
LUAEXT_DEFINE_TRACE_ACCESSORS(DevelopGravity_LuaExt_Exception_LuaLogicException)
