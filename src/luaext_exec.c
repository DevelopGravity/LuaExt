/*
 * luaext — running Lua and reading its results.
 *
 * Scaffold only: the build wiring and the header contract land first so the
 * execution and PHP-bridge subsystems can be written against a tree that
 * already compiles. Bodies arrive with Wave 2b.
 */

#include "luaext_exec.h"

#include "luaext_convert.h"
#include "luaext_error.h"

bool luaext_exec_load(luaext_sandbox *sandbox, const char *code, size_t code_len,
					  const char *chunk_name, bool allow_binary)
{
	(void)sandbox;
	(void)code;
	(void)code_len;
	(void)chunk_name;
	(void)allow_binary;
	return false;
}

bool luaext_exec_pcall(luaext_sandbox *sandbox, int func_index, zval *args, uint32_t argc,
					   zval *return_value)
{
	(void)sandbox;
	(void)func_index;
	(void)args;
	(void)argc;
	(void)return_value;
	return false;
}

bool luaext_exec_push_path(luaext_sandbox *sandbox, const char *path, size_t path_len)
{
	(void)sandbox;
	(void)path;
	(void)path_len;
	return false;
}

bool luaext_exec_assign_path(luaext_sandbox *sandbox, const char *path, size_t path_len)
{
	(void)sandbox;
	(void)path;
	(void)path_len;
	return false;
}

void luaext_exec_make_function(luaext_sandbox *sandbox, zval *sandbox_zv, zval *return_value)
{
	(void)sandbox;
	(void)sandbox_zv;
	(void)return_value;
}
