/*
 * luaext — the bridge to the host's FileSystem. See luaext_vfs.h for the rules.
 */

#include "luaext_vfs.h"

#include "luaext_error.h"
#include "luaext_timers.h"

#include <lauxlib.h>
#include <lua.h>

#include <Zend/zend_exceptions.h>
#include <Zend/zend_interfaces.h>
#include <Zend/zend_object_handlers.h>

#include <string.h>

bool luaext_vfs_available(const luaext_sandbox *sandbox)
{
	return sandbox != NULL && Z_TYPE(sandbox->filesystem_zv) == IS_OBJECT;
}

bool luaext_vfs_writable(const luaext_sandbox *sandbox)
{
	return luaext_vfs_available(sandbox) && luaext_has_cap(&sandbox->policy, LUAEXT_CAP_VFS_WRITE);
}

/* Same shape as the output sink's property reader: the config object is a
 * strict-properties final class, so a miss here is a build error rather than a
 * runtime condition. */
static zval *luaext_vfs_property(zval *config, const char *name)
{
	if (config == NULL || Z_TYPE_P(config) != IS_OBJECT) {
		return NULL;
	}

	return zend_read_property(luaext_ce_sandbox_config, Z_OBJ_P(config), name, strlen(name), 1,
							  NULL);
}

bool luaext_vfs_init_from_config(luaext_sandbox *sandbox, zval *config)
{
	return luaext_vfs_init(sandbox, luaext_vfs_property(config, "filesystem"));
}

bool luaext_vfs_init(luaext_sandbox *sandbox, zval *filesystem)
{
	ZVAL_UNDEF(&sandbox->filesystem_zv);
	sandbox->vfs_ranged = false;
	sandbox->vfs_ops_this_call = 0;

	if (filesystem == NULL || Z_TYPE_P(filesystem) != IS_OBJECT) {
		return true;
	}

	if (!luaext_has_cap(&sandbox->policy, LUAEXT_CAP_VFS)) {
		/* Configured but not granted. Not an error -- luaext_config_check()
		 * already refuses the reverse -- and simply nothing is attached. */
		return true;
	}

	ZVAL_COPY(&sandbox->filesystem_zv, filesystem);

	/*
	 * Asked once at construction rather than per call. Whether the backend can
	 * seek decides between streaming and whole-file buffering for the sandbox's
	 * whole life; re-deriving it on every read would let a backend answer
	 * differently over time, which no caller here is prepared for.
	 */
	sandbox->vfs_ranged =
		instanceof_function(Z_OBJCE(sandbox->filesystem_zv), luaext_ce_ranged_file_system) != 0;

	return true;
}

void luaext_vfs_shutdown(luaext_sandbox *sandbox)
{
	if (sandbox == NULL) {
		return;
	}

	if (Z_TYPE(sandbox->filesystem_zv) == IS_OBJECT) {
		zval_ptr_dtor(&sandbox->filesystem_zv);
	}

	ZVAL_UNDEF(&sandbox->filesystem_zv);
	sandbox->vfs_ranged = false;
}

void luaext_vfs_begin_call(luaext_sandbox *sandbox)
{
	if (sandbox != NULL) {
		sandbox->vfs_ops_this_call = 0;
	}
}

bool luaext_vfs_charge_operation(lua_State *L, luaext_sandbox *sandbox)
{
	uint32_t cap = sandbox->policy.vfs_quota.max_operations;

	sandbox->vfs_operations++;
	sandbox->vfs_ops_this_call++;

	if (cap != 0 && sandbox->vfs_ops_this_call > cap) {
		/*
		 * Fatal rather than `nil, message`. A script that could catch this
		 * would call again, and the quota exists to bound how much host work one
		 * script call can demand -- a bound a script may decline is not one.
		 */
		luaext_error_raise(L, LUAEXT_ERR_VFS, true,
						   "This call has already made %u filesystem operation(s), which is its "
						   "VfsQuota::$maxOperations",
						   (unsigned int)cap);
		return false;
	}

	return true;
}

luaext_vfs_result luaext_vfs_call(lua_State *L, luaext_sandbox *sandbox, const char *method,
								  uint32_t argc, zval *args, zval *result, zend_string **refusal)
{
	zend_string *name;
	zend_function *fn;
	uint8_t paused = 0;
	bool billed_wall;

	ZVAL_UNDEF(result);

	if (refusal != NULL) {
		*refusal = NULL;
	}

	if (!luaext_vfs_available(sandbox)) {
		luaext_error_raise(L, LUAEXT_ERR_VFS, false, "%s",
						   "This sandbox has no filesystem: SandboxConfig::$filesystem is null");
		return LUAEXT_VFS_FAILED;
	}

	if (!luaext_vfs_charge_operation(L, sandbox)) {
		return LUAEXT_VFS_FAILED;
	}

	/*
	 * The wall clock pauses across a backend call unless the host asked for it
	 * to be billed. CPU is billed either way and deliberately: the backend runs
	 * on this thread, so its CPU IS the script's, and billing more rather than
	 * less is the safe direction. See SECURITY.md on what a hanging backend
	 * costs when billWallTime is left off.
	 */
	billed_wall = sandbox->policy.vfs_quota.bill_wall_time;

	if (!billed_wall) {
		paused = luaext_timers_pause(sandbox, LUAEXT_TIMER_WALL) ? LUAEXT_TIMER_WALL : 0;
	}

	/*
	 * Looked up and called directly rather than through zend_call_method(),
	 * which caps at two arguments -- RangedFileSystem::readRange() takes three.
	 * The interface guarantees the method exists, so a miss is a build-time
	 * mistake here rather than a runtime condition, but it is still checked: a
	 * null dereference is a worse way to learn about it.
	 */
	name = zend_string_init_interned(method, strlen(method), 0);
	fn = zend_hash_find_ptr(&Z_OBJCE(sandbox->filesystem_zv)->function_table, name);
	zend_string_release(name);

	if (fn == NULL) {
		if (paused != 0) {
			luaext_timers_resume(sandbox, paused);
		}

		luaext_error_raise(L, LUAEXT_ERR_VFS, false,
						   "The configured FileSystem does not implement %s()", method);
		return LUAEXT_VFS_FAILED;
	}

	zend_call_known_instance_method(fn, Z_OBJ(sandbox->filesystem_zv), result, argc, args);

	if (paused != 0) {
		luaext_timers_resume(sandbox, paused);
	}

	if (!EG(exception)) {
		return LUAEXT_VFS_OK;
	}

	/*
	 * An exact allowlist, never a catch-all. A VfsError is the backend saying
	 * something a script is entitled to hear and handle -- not found, refused,
	 * out of space -- and becomes `nil, message`. Everything else is the host
	 * failing: a database down, a bug, an assertion. Converting those into a
	 * script-visible nil would let a script probe for outages and swallow them,
	 * and would hide a real fault from the caller who could act on it.
	 */
	if (instanceof_function(EG(exception)->ce, luaext_ce_vfs_error)) {
		if (refusal != NULL) {
			zval message;

			ZVAL_UNDEF(&message);
			zend_read_property_ex(zend_get_exception_base(EG(exception)), EG(exception),
								  ZSTR_KNOWN(ZEND_STR_MESSAGE), 1, &message);

			if (Z_TYPE(message) == IS_STRING) {
				*refusal = zend_string_copy(Z_STR(message));
			}
		}

		zend_clear_exception();

		if (Z_TYPE_P(result) != IS_UNDEF) {
			zval_ptr_dtor(result);
			ZVAL_UNDEF(result);
		}

		return LUAEXT_VFS_REFUSED;
	}

	/*
	 * Left pending, not converted. luaext_phpcall.c already owns the rule that a
	 * host exception reaches the caller as the class it threw rather than as a
	 * degraded string, and the same applies here -- the exception travels back
	 * through the boundary intact.
	 */
	if (Z_TYPE_P(result) != IS_UNDEF) {
		zval_ptr_dtor(result);
		ZVAL_UNDEF(result);
	}

	return LUAEXT_VFS_FAILED;
}

bool luaext_vfs_check_range(lua_State *L, const luaext_sandbox *sandbox, lua_Integer offset,
							lua_Integer length, uint64_t *end_position)
{
	uint64_t end;
	size_t max_file = sandbox->policy.vfs_quota.max_file_bytes;

	/*
	 * Both signed, both chosen by the script. A negative value passed onward
	 * becomes an enormous size_t at the first cast, so it is refused here rather
	 * than reinterpreted anywhere below.
	 */
	if (offset < 0) {
		luaext_error_raise(L, LUAEXT_ERR_VFS, false, "A file offset cannot be negative (got %lld)",
						   (long long)offset);
		return false;
	}

	if (length < 0) {
		luaext_error_raise(L, LUAEXT_ERR_VFS, false, "A byte count cannot be negative (got %lld)",
						   (long long)length);
		return false;
	}

	/*
	 * Checked BEFORE the addition is used for anything. offset + length wrapping
	 * past UINT64_MAX would produce a small number that satisfies every quota
	 * test below while describing a range that does not fit in the universe.
	 */
	if ((uint64_t)offset > UINT64_MAX - (uint64_t)length) {
		luaext_error_raise(L, LUAEXT_ERR_VFS, false,
						   "A file range starting at %lld for %lld byte(s) does not fit in a "
						   "64-bit position",
						   (long long)offset, (long long)length);
		return false;
	}

	end = (uint64_t)offset + (uint64_t)length;

	/*
	 * The END position, not the length. A one-byte write at offset 2^40 asks the
	 * backend to hold a one terabyte file, and judging it by the single byte it
	 * carries is how a sparse write walks straight through a per-file quota.
	 */
	if (max_file != 0 && end > (uint64_t)max_file) {
		luaext_error_raise(L, LUAEXT_ERR_VFS, false,
						   "A file range ending at byte %llu exceeds the %zu byte "
						   "VfsQuota::$maxFileBytes",
						   (unsigned long long)end, max_file);
		return false;
	}

	if (end_position != NULL) {
		*end_position = end;
	}

	return true;
}
