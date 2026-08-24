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
 * Call a function at `func_index` with `argc` zvals, leaving nothing on the
 * stack. Results are converted into `return_value` as a list.
 *
 * This is the single choke point where Lua errors become PHP exceptions, so it
 * is also where the traceback handler is installed.
 */
bool luaext_exec_pcall(luaext_sandbox *sandbox, int func_index, zval *args, uint32_t argc,
					   zval *return_value);

/*
 * Resolve a dotted path such as "a.b.c" against the globals table, pushing the
 * value it names. Pushes nil for a missing leaf; fails only when an
 * intermediate component exists but cannot be indexed.
 */
bool luaext_exec_push_path(luaext_sandbox *sandbox, const char *path, size_t path_len);

/*
 * Assign the value on the top of the stack to a dotted path, creating
 * intermediate tables as needed. Pops the value either way.
 */
bool luaext_exec_assign_path(luaext_sandbox *sandbox, const char *path, size_t path_len);

/*
 * Wrap the function on the top of the stack in a LuaFunction object. Pops it.
 */
void luaext_exec_make_function(luaext_sandbox *sandbox, zval *sandbox_zv, zval *return_value);

#endif /* LUAEXT_EXEC_H */
