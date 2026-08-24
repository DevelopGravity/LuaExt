/*
 * luaext — the PHP side of the boundary.
 *
 * Scaffold only: see luaext_exec.c. Bodies arrive with Wave 2b.
 */

#include "luaext_phpcall.h"

#include "luaext_convert.h"
#include "luaext_error.h"

bool luaext_phpcall_push(luaext_sandbox *sandbox, zval *callable, const char *name)
{
	(void)sandbox;
	(void)callable;
	(void)name;
	return false;
}

bool luaext_phpcall_register_table(luaext_sandbox *sandbox, const char *name, size_t name_len,
								   HashTable *functions)
{
	(void)sandbox;
	(void)name;
	(void)name_len;
	(void)functions;
	return false;
}

HashTable *luaext_phpcall_collect_methods(zval *instance, HashTable *allowlist)
{
	(void)instance;
	(void)allowlist;
	return NULL;
}
