/*
 * luaext — errors across the PHP/Lua boundary.
 *
 * The load-bearing distinction is fatal vs catchable. A script may catch its
 * own mistakes with pcall; it must never be able to catch a limit breach or a
 * host failure, or every guarantee this sandbox makes becomes advisory.
 *
 * That is enforced by representing errors as a full userdata with a private,
 * __metatable-protected metatable. Lua code cannot construct userdata, so a
 * fatal error cannot be forged, and it cannot be stripped of its marker to
 * look catchable.
 */

#ifndef LUAEXT_ERROR_H
#define LUAEXT_ERROR_H

#include "luaext_types.h"

/* Install the error metatable in the registry. Called once per lua_State. */
void luaext_error_init(luaext_sandbox *sandbox);

/*
 * Push a luaext_error_ud describing `kind` and raise it with lua_error().
 * Does not return.
 *
 * `fatal` decides whether the sandbox's own pcall/xpcall/coroutine.resume are
 * permitted to catch it. Limits, host failures and conversion faults are
 * always fatal.
 */
ZEND_COLD ZEND_NORETURN void luaext_error_raise(lua_State *L, luaext_err_kind kind, bool fatal,
												const char *format, ...);

/*
 * Wrap a pending PHP exception as a Lua error and raise it. Does not return.
 *
 * A RuntimeError (or subclass) becomes catchable; anything else becomes fatal.
 * Either way the original exception object is retained on the error userdata,
 * so if it reaches PHP uncaught it is rethrown unchanged rather than degraded
 * to its message text.
 *
 * Clears EG(exception): from here the error travels as a Lua value.
 */
ZEND_COLD ZEND_NORETURN void luaext_error_raise_from_exception(lua_State *L);

/*
 * Whether the value at `index` is one of our error userdata, and if so whether
 * it is fatal. Never allocates and never raises, so it is safe to call from an
 * error handler.
 */
bool luaext_error_is_ours(lua_State *L, int index);
bool luaext_error_is_fatal(lua_State *L, int index);

/*
 * Turn the Lua error value at the top of the stack into a thrown PHP
 * exception, then pop it.
 *
 * `status` is the lua_pcall/lua_resume result, used to classify errors Lua
 * raised itself (LUA_ERRSYNTAX, LUA_ERRMEM, LUA_ERRERR). A retained PHP
 * exception is rethrown as-is with the Lua traceback attached; anything else
 * is mapped to the matching class from the exception hierarchy.
 */
void luaext_error_throw_from_lua(luaext_sandbox *sandbox, lua_State *L, int status);

/*
 * The message handler to pass as lua_pcall's `msgh`. Attaches a structured
 * traceback to the error value while the erroring stack still exists.
 *
 * Runs with the memory limit temporarily lifted: reporting that a script ran
 * out of memory must not itself fail for want of memory.
 */
int luaext_error_traceback_handler(lua_State *L);

/*
 * Push the structured traceback captured by the handler as a PHP array
 * matching LuaThrowable::getLuaTrace(), or null when there is none.
 */
void luaext_error_trace_to_zval(lua_State *L, int index, zval *out);

#endif /* LUAEXT_ERROR_H */
