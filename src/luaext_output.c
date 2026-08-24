/*
 * luaext — where a script's output goes.
 *
 * SCAFFOLD. The four Sandbox output methods delegate here and currently refuse,
 * which is the same answer they gave before the delegation existed.
 *
 * When this is built, the two properties that matter beyond plumbing:
 *
 *   Overflow under OverflowBehavior::Fail raises a FATAL OutputLimitError. A
 *   script must not be able to pcall its way past its own output budget.
 *
 *   The buffer is charged against memoryBytes through luaext_alloc_charge().
 *   It is host memory that lua_Alloc never sees, so without that a script who
 *   cannot allocate a large Lua string can still exhaust the same budget by
 *   printing one.
 */

#include "luaext_output.h"

#include "luaext_alloc.h"
#include "luaext_error.h"

#include <Zend/zend_exceptions.h>

bool luaext_output_init(luaext_sandbox *sandbox, zval *config)
{
	(void)sandbox;
	(void)config;
	return true;
}

void luaext_output_shutdown(luaext_sandbox *sandbox)
{
	(void)sandbox;
}

bool luaext_output_write(luaext_sandbox *sandbox, const char *data, size_t length)
{
	(void)sandbox;
	(void)data;
	(void)length;
	return true;
}

static void luaext_output_unavailable(const char *what)
{
	zend_throw_error(NULL, "DevelopGravity\\LuaExt\\Sandbox::%s() is not implemented yet", what);
}

zend_string *luaext_output_get(luaext_sandbox *sandbox, bool take)
{
	(void)sandbox;
	luaext_output_unavailable(take ? "takeOutput" : "getOutput");
	return NULL;
}

size_t luaext_output_length(const luaext_sandbox *sandbox)
{
	(void)sandbox;
	luaext_output_unavailable("getOutputLength");
	return 0;
}

bool luaext_output_truncated(const luaext_sandbox *sandbox)
{
	(void)sandbox;
	luaext_output_unavailable("isOutputTruncated");
	return false;
}
