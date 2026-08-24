/*
 * luaext — running Lua and reading its results.
 *
 * Everything here assumes the caller is on the sandbox's owning thread and that
 * the sandbox is open; the ZEND_METHOD wrappers in luaext_sandbox.c check both
 * before delegating.
 */

#ifndef LUAEXT_EXEC_H
#define LUAEXT_EXEC_H

#include "luaext_types.h"

/*
 * Compile source into a function on the top of the stack.
 *
 * Text mode only unless `allow_binary`, which the caller gates on the
 * loadBytecode capability -- luaL_loadbufferx's mode argument is the only thing
 * standing between an untrusted string and the bytecode verifier's absence.
 *
 * Returns false with a typed exception already thrown, stack unchanged.
 */
bool luaext_exec_load(luaext_sandbox *sandbox, const char *code, size_t code_len,
					  const char *chunk_name, bool allow_binary);

/*
 * Push a PHP value, leaving it on the top of the stack.
 *
 * luaext_convert_push_zval() reports failure by raising, which is right for a
 * caller already running inside a protected call and wrong for a PHP method
 * body: a longjmp there would unwind past the engine. This runs it under
 * lua_pcall, so a refusal arrives as a thrown PHP exception with the stack
 * exactly as it was found.
 */
bool luaext_exec_push_value(luaext_sandbox *sandbox, zval *value);

/*
 * Call a function at `func_index` with `argc` zvals, leaving nothing on the
 * stack -- the function and anything above it go with the call. Results are
 * converted into `return_value` as a list.
 *
 * This is the single choke point where Lua errors become PHP exceptions, so it
 * is also where the traceback handler is installed.
 *
 * Returns false with a typed exception already thrown; `return_value` is then
 * left untouched for the caller's RETURN_THROWS().
 */
bool luaext_exec_pcall(luaext_sandbox *sandbox, int func_index, zval *args, uint32_t argc,
					   zval *return_value);

/*
 * Resolve a dotted path such as "a.b.c" against the globals table, pushing the
 * value it names. Pushes nil for a missing leaf, and for anything below a
 * missing intermediate; fails only when an intermediate component exists but
 * cannot be indexed, and when the path itself names nothing ("", "a..b").
 *
 * Every lookup is raw. Both of these run after a call has finished, so an
 * __index metamethod here would execute untrusted code with nothing bounding
 * it.
 *
 * Returns false with a typed exception already thrown, stack unchanged.
 */
bool luaext_exec_push_path(luaext_sandbox *sandbox, const char *path, size_t path_len);

/*
 * Assign the value on the top of the stack to a dotted path, creating
 * intermediate tables as needed. Pops the value either way.
 *
 * Raw, so __newindex cannot intercept a host assignment and so storing nil
 * deletes the key. Returns false with a typed exception already thrown.
 */
bool luaext_exec_assign_path(luaext_sandbox *sandbox, const char *path, size_t path_len);

/*
 * Wrap the function on the top of the stack in a LuaFunction object. Pops it.
 *
 * The handle takes a registry slot and a reference to `sandbox_zv`, in that
 * order: the slot is returned to a freelist that lives in the sandbox, so the
 * handle must not be able to outlive it. Leaves `return_value` untouched and
 * throws when the slot cannot be taken, so callers check EG(exception).
 */
void luaext_exec_make_function(luaext_sandbox *sandbox, zval *sandbox_zv, zval *return_value);

#endif /* LUAEXT_EXEC_H */
