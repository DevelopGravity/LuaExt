/*
 * luaext — the bridge to the host's FileSystem. See luaext_vfs.h for the rules.
 */

#include "luaext_vfs.h"

#include "luaext_error.h"
#include "luaext_timers.h"
#include "luaext_vfs_path.h"

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

/*
 * The body of luaext_vfs_call, with charging made optional.
 *
 * The quota exists to bound what a SCRIPT can demand of the host. The file-count
 * walk is not that: it is this layer's own bookkeeping, run once per sandbox to
 * answer a question the script never asked. Charging it would make creating a
 * file spend a budget the script cannot see, and -- worse -- would put a raising
 * call inside a loop that holds zvals, so exhausting the quota mid-walk would
 * longjmp past every dtor in it.
 */
static luaext_vfs_result luaext_vfs_call_maybe_charged(lua_State *L, luaext_sandbox *sandbox,
													   const char *method, uint32_t argc,
													   zval *args, zval *result,
													   zend_string **refusal, bool charge)
{
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

	if (charge && !luaext_vfs_charge_operation(L, sandbox)) {
		return LUAEXT_VFS_FAILED;
	}

	/* Still counted for SandboxStats::$vfsOperations either way: the host really
	 * did make the call, whoever asked for it. */
	if (!charge) {
		sandbox->vfs_operations++;
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
	/*
	 * Lowercased, because that is how a class's function_table is keyed. Without
	 * it every single-word method resolves and every camelCase one does not --
	 * exists() and read() would work while readRange() reported a FileSystem
	 * that "does not implement" a method it plainly does.
	 *
	 * _lc() rather than lowering a zend_string ourselves: it hashes the name
	 * case-insensitively straight off the C string, so this allocates nothing at
	 * all. The version that did allocate leaked one zend_string PER VFS CALL --
	 * zend_string_tolower_ex() does not take ownership of its argument, so
	 * releasing only its result freed the copy and never the original. Invisible
	 * to a release build and to macOS `leaks`; PHP's debug allocator reported 48
	 * of them in a single test.
	 */
	fn = zend_hash_str_find_ptr_lc(&Z_OBJCE(sandbox->filesystem_zv)->function_table, method,
								   strlen(method));

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
			zval scratch;
			zval *message;

			/*
			 * The RETURN VALUE, not the scratch zval. zend_read_property_ex only
			 * writes through its last argument for properties it has to
			 * materialise; for an ordinary declared property -- which
			 * Exception::$message is -- it hands back a pointer to the property
			 * itself and leaves the scratch untouched. Reading the scratch left
			 * every refusal looking like it carried no message, which sent it
			 * down the host-failure path and turned a `nil, message` a script
			 * should have handled into an uncatchable error.
			 */
			ZVAL_UNDEF(&scratch);
			message = zend_read_property_ex(zend_get_exception_base(EG(exception)), EG(exception),
											ZSTR_KNOWN(ZEND_STR_MESSAGE), 1, &scratch);

			if (message != NULL && Z_TYPE_P(message) == IS_STRING) {
				*refusal = zend_string_copy(Z_STR_P(message));
			}

			zval_ptr_dtor(&scratch);
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

luaext_vfs_result luaext_vfs_call(lua_State *L, luaext_sandbox *sandbox, const char *method,
								  uint32_t argc, zval *args, zval *result, zend_string **refusal)
{
	return luaext_vfs_call_maybe_charged(L, sandbox, method, argc, args, result, refusal, true);
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

/*
 * A zend_string owned by Lua's collector rather than by the C frame that made
 * it.
 *
 * WHY THIS EXISTS. Everything a script does through the VFS ends in a call to
 * the backend, and every one of those calls can raise: a quota refusal, a
 * FileSystem that is gone, a method the interface promises and the class does
 * not have. luaext_error_raise() ends in lua_error(), which longjmps, and a
 * longjmp runs no cleanup whatsoever. A frame holding a string across such a
 * call leaks it, on precisely the paths that only execute when something has
 * already gone wrong.
 *
 * That is not a hazard someone might one day hit. It has been found twice:
 *
 *   - io.open() leaked its canonical path on every maxOpenHandles, maxFiles and
 *     maxOperations refusal;
 *   - a ranged file:write() leaked the COPY of the bytes it hands writeRange(),
 *     which is script-sized -- a loop writing 1 MB chunks leaked a megabyte on
 *     every refusal.
 *
 * Both were silent in a release build and under macOS `leaks`, which sees the
 * request arena freed wholesale and calls it clean. PHP's debug allocator is the
 * only thing that reports them, which is why this file's tests earn their place
 * on the debug and sanitizer legs rather than here.
 *
 * Handing the string to Lua ends the whole class instead of one instance of it.
 * The box lives in the calling C function's own stack frame, so an unwind of any
 * depth makes it garbage; no exit has to remember to release, and no exit added
 * later can forget to. It also costs the callers nothing to hold onto -- the box
 * is anonymous, and Lua discards the frame's leftovers when the function
 * returns.
 */
typedef struct {
	zend_string *held;
} luaext_vfs_string_ud;

static int luaext_vfs_string_release(lua_State *L)
{
	luaext_vfs_string_ud *box = (luaext_vfs_string_ud *)lua_touserdata(L, 1);

	if (box != NULL && box->held != NULL) {
		zend_string_release(box->held);
		box->held = NULL;
	}

	return 0;
}

/*
 * Push the metatable every anchored-string box in this state shares.
 *
 * Its own registry key, not luaext_key_zvalmt: a metatable *is* its __gc, so
 * giving a box the closure storage's finaliser would have that __gc read a
 * zend_fcall_info_cache out of a zend_string. __metatable is false for the same
 * reason it is on the others -- a script that could read this table back could
 * replace __gc, or stamp it onto a value of its own and hand the collector a
 * pointer we never allocated.
 */
static void luaext_vfs_string_metatable(lua_State *L)
{
	if (lua_rawgetp(L, LUA_REGISTRYINDEX, &luaext_key_pathmt) == LUA_TTABLE) {
		return;
	}

	lua_pop(L, 1);
	lua_createtable(L, 0, 2);

	lua_pushcfunction(L, luaext_vfs_string_release);
	lua_setfield(L, -2, "__gc");

	lua_pushboolean(L, 0);
	lua_setfield(L, -2, "__metatable");

	lua_pushvalue(L, -1);
	lua_rawsetp(L, LUA_REGISTRYINDEX, &luaext_key_pathmt);
}

/*
 * Push an empty box and return it, armed and ready to be filled.
 *
 * THE ORDER IS LOAD BEARING. The box goes up empty, with its metatable, BEFORE
 * the string exists: setting a metatable that carries __gc is what marks a
 * userdata for finalisation, so doing it the other way round leaves a window in
 * which the string is owned by nothing -- which is the bug this exists to
 * prevent.
 *
 * Returns NULL, with an error raised, once the state is closing: a box built
 * then would never be collected. lua_close() sets GCSTPCLS for its whole run and
 * luaC_checkfinalizer() returns early while that bit is set, so an object created
 * inside a finaliser is never added to the finobj list and its __gc NEVER RUNS --
 * the same trap the error subsystem documents. Reaching here during close means
 * a script __gc touching the VFS, which is already torn down by then, so the
 * call had no future anyway.
 */
static luaext_vfs_string_ud *luaext_vfs_push_box(lua_State *L, luaext_sandbox *sandbox)
{
	luaext_vfs_string_ud *box;

	if (sandbox == NULL || sandbox->closed) {
		luaext_error_raise(L, LUAEXT_ERR_VFS, false, "%s",
						   "This sandbox is closing: its filesystem is no longer reachable");
		return NULL;
	}

	luaL_checkstack(L, 3, "luaext: no stack to hold a string for the filesystem");

	box = (luaext_vfs_string_ud *)lua_newuserdatauv(L, sizeof(*box), 0);
	box->held = NULL;

	luaext_vfs_string_metatable(L);
	lua_setmetatable(L, -2);

	return box;
}

zend_string *luaext_vfs_anchor_string(lua_State *L, luaext_sandbox *sandbox, const char *data,
									  size_t length)
{
	luaext_vfs_string_ud *box = luaext_vfs_push_box(L, sandbox);

	if (box == NULL) {
		return NULL;
	}

	box->held = zend_string_init(data, length, 0);

	return box->held;
}

/*
 * Canonicalise a script-supplied path into a zend_string the backend may see.
 *
 * The result is BORROWED: it belongs to a box on the Lua stack, and the caller
 * must neither release it nor keep it past its own return. See the box above for
 * why ownership sits there.
 *
 * The buffer is the caller's, sized from the input, because the canonical form
 * is never longer than the input plus a leading slash -- canonicalisation only
 * removes components.
 */
zend_string *luaext_vfs_path_from_lua(lua_State *L, luaext_sandbox *sandbox, int index)
{
	const char *raw;
	size_t raw_len;
	char *buffer;
	size_t out_len = 0;
	luaext_vfs_path_status status;
	luaext_vfs_string_ud *box;

	raw = luaL_checklstring(L, index, &raw_len);

	/*
	 * Bounded before allocating. A quota of zero means "no bound from the
	 * quota", so the buffer bound below is what remains, and it is the same
	 * ceiling the path module documents.
	 */
	if (raw_len > LUAEXT_VFS_PATH_MAX_INPUT) {
		luaext_error_raise(L, LUAEXT_ERR_VFS, false,
						   "A path of %zu bytes is longer than this sandbox will canonicalise",
						   raw_len);
		return NULL;
	}

	box = luaext_vfs_push_box(L, sandbox);

	if (box == NULL) {
		return NULL;
	}

	buffer = emalloc(raw_len + 2);

	status = luaext_vfs_path_canonical(raw, raw_len, buffer, raw_len + 2, &out_len,
									   sandbox->policy.vfs_quota.max_path_length,
									   sandbox->policy.vfs_quota.max_path_depth);

	if (status != LUAEXT_VFS_PATH_OK) {
		efree(buffer);

		/*
		 * Fatal rather than `nil, message`, and deliberately so for the escape
		 * case: a script probing for the root's real location learns nothing from
		 * a catchable error it can loop over. The others join it because a
		 * malformed path is a bug in the script, not a condition of the store.
		 */
		luaext_error_raise(L, LUAEXT_ERR_VFS, false, "This path cannot be used: %s",
						   luaext_vfs_path_reason(status));
		return NULL;
	}

	box->held = zend_string_init(buffer, out_len, 0);
	efree(buffer);

	return box->held;
}

void luaext_vfs_note_bytes(luaext_sandbox *sandbox, size_t bytes)
{
	sandbox->vfs_bytes += (uint64_t)bytes;
}

void luaext_vfs_note_file_removed(luaext_sandbox *sandbox)
{
	/* Only meaningful once something has counted. Before that the first
	 * creation does a full walk and sees the post-delete truth anyway. */
	if (sandbox->vfs_file_count_known && sandbox->vfs_file_count > 0) {
		sandbox->vfs_file_count--;
	}
}

/* -------------------------------------------------------------------------
 * Open files
 * ---------------------------------------------------------------------- */

/*
 * The strong table of open handles, keyed by the handle userdata.
 *
 * Deliberately not weak, unlike the coroutine table. A coroutine that becomes
 * garbage is finished and holds nothing the host needs; a dropped file handle
 * may still be carrying unwritten bytes, and letting the collector decide when
 * those reach the backend would make a write's durability depend on GC timing.
 */
static void luaext_vfs_push_handles(lua_State *L)
{
	if (lua_rawgetp(L, LUA_REGISTRYINDEX, &luaext_key_handles) == LUA_TTABLE) {
		return;
	}

	lua_pop(L, 1);
	lua_createtable(L, 0, 8);

	lua_pushvalue(L, -1);
	lua_rawsetp(L, LUA_REGISTRYINDEX, &luaext_key_handles);
}

/*
 * Charge or refund a handle's buffer against both budgets.
 *
 * A buffer is the sandbox's memory as much as a Lua string is, so it answers to
 * Limits::$memoryBytes as well as to the VFS's own maxTotalBytes. Charging only
 * the VFS quota would let a script hold eight megabytes of file buffers inside a
 * one megabyte sandbox.
 */
bool luaext_vfs_charge_buffer_public(lua_State *L, luaext_sandbox *sandbox, size_t bytes)
{
	size_t cap = sandbox->policy.vfs_quota.max_total_bytes;

	if (cap != 0 && bytes > cap - sandbox->vfs_buffered_bytes) {
		luaext_error_raise(L, LUAEXT_ERR_VFS, true,
						   "Buffering %zu more byte(s) would pass the %zu byte "
						   "VfsQuota::$maxTotalBytes",
						   bytes, cap);
		return false;
	}

	sandbox->vfs_buffered_bytes += bytes;

	return true;
}

static void luaext_vfs_refund_buffer(luaext_sandbox *sandbox, size_t bytes)
{
	/* Clamped rather than trusted. The counter is maintained across paths that
	 * can unwind, and a wrapped size_t here would read as an enormous budget. */
	sandbox->vfs_buffered_bytes =
		bytes > sandbox->vfs_buffered_bytes ? 0 : sandbox->vfs_buffered_bytes - bytes;
}

/* Release what a handle owns and stop counting it. Never calls the backend. */
static void luaext_vfs_handle_release(luaext_sandbox *sandbox, luaext_vfs_handle *handle)
{
	if (handle->buffer != NULL) {
		luaext_vfs_refund_buffer(sandbox, ZSTR_LEN(handle->buffer));
		zend_string_release(handle->buffer);
		handle->buffer = NULL;
	}

	if (handle->path != NULL) {
		zend_string_release(handle->path);
		handle->path = NULL;
	}

	if (!handle->closed) {
		handle->closed = true;

		if (sandbox->vfs_open_handles > 0) {
			sandbox->vfs_open_handles--;
		}
	}
}

void luaext_vfs_handle_gc(luaext_sandbox *sandbox, luaext_vfs_handle *handle)
{
	luaext_vfs_handle_release(sandbox, handle);
}

/* Parse Lua's mode string. Returns false for anything that is not one of the
 * six upstream accepts, rather than guessing at an intent. */
static bool luaext_vfs_parse_mode(const char *mode, bool *readable, bool *writable, bool *append,
								  bool *truncate)
{
	size_t index = 0;

	if (mode == NULL || mode[0] == '\0') {
		return false;
	}

	*readable = false;
	*writable = false;
	*append = false;
	*truncate = false;

	switch (mode[0]) {
	case 'r':
		*readable = true;
		break;
	case 'w':
		*writable = true;
		*truncate = true;
		break;
	case 'a':
		*writable = true;
		*append = true;
		break;
	default:
		return false;
	}

	for (index = 1; mode[index] != '\0'; index++) {
		if (mode[index] == '+') {
			*readable = true;
			*writable = true;
		} else if (mode[index] != 'b') {
			/* 'b' is accepted and ignored: the VFS moves bytes either way, so
			 * there is no text translation for it to select. */
			return false;
		}
	}

	return true;
}

/* -------------------------------------------------------------------------
 * VfsQuota::$maxFiles
 * ---------------------------------------------------------------------- */

/*
 * Count the files under `dir`, recursively, stopping once `ceiling` is passed.
 *
 * FileSystem::list() reports direct children only, and nothing in the interface
 * says which of them are directories, so each entry costs a stat(). That is why
 * the walk stops early: the caller only ever needs to know whether the count has
 * reached the quota, never what it is exactly, and a namespace far larger than
 * the quota must not cost proportionally more to refuse.
 *
 * Depth is bounded by the same quota the path canonicaliser uses, so a backend
 * reporting a cyclic namespace cannot recurse forever.
 */
static bool luaext_vfs_count_files(lua_State *L, luaext_sandbox *sandbox, const char *dir,
								   uint32_t ceiling, uint32_t depth, uint32_t *count)
{
	zval args[1];
	zval result;
	zend_string *refusal = NULL;
	zval *entry;
	uint32_t max_depth = sandbox->policy.vfs_quota.max_path_depth;

	if (max_depth != 0 && depth > max_depth) {
		return true;
	}

	/*
	 * Everything from here to the matching END holds zvals. A raise inside it is
	 * a longjmp past every dtor below, so the assertion says out loud what the
	 * uncharged call above quietly guarantees.
	 */
	LUAEXT_NO_RAISE_BEGIN(L);

	ZVAL_STR(&args[0], zend_string_init(dir, strlen(dir), 0));

	if (luaext_vfs_call_maybe_charged(L, sandbox, "list", 1, args, &result, &refusal, false) !=
		LUAEXT_VFS_OK) {
		zval_ptr_dtor(&args[0]);

		if (refusal != NULL) {
			/* A directory the backend will not list contributes nothing rather
			 * than aborting the count -- the same "treat it as absent" rule the
			 * module search uses. */
			zend_string_release(refusal);
			LUAEXT_NO_RAISE_END(L);
			return true;
		}

		LUAEXT_NO_RAISE_END(L);
		return false;
	}

	zval_ptr_dtor(&args[0]);

	if (Z_TYPE(result) != IS_ARRAY) {
		zval_ptr_dtor(&result);
		LUAEXT_NO_RAISE_END(L);
		return true;
	}

	ZEND_HASH_FOREACH_VAL(Z_ARRVAL(result), entry)
	{
		smart_str child = {0};
		zval stat_args[1];
		zval stat_result;
		bool is_dir = false;

		if (*count > ceiling) {
			break;
		}

		if (Z_TYPE_P(entry) != IS_STRING) {
			continue;
		}

		smart_str_appends(&child, dir);

		if (strcmp(dir, "/") != 0) {
			smart_str_appendc(&child, '/');
		}

		smart_str_appendl(&child, Z_STRVAL_P(entry), Z_STRLEN_P(entry));
		smart_str_0(&child);

		ZVAL_STR(&stat_args[0], zend_string_copy(child.s));

		if (luaext_vfs_call_maybe_charged(L, sandbox, "stat", 1, stat_args, &stat_result, &refusal,
										  false) == LUAEXT_VFS_OK) {
			if (Z_TYPE(stat_result) == IS_OBJECT) {
				zval *flag = zend_read_property(luaext_ce_file_stat, Z_OBJ(stat_result),
												"isDirectory", strlen("isDirectory"), 1, NULL);

				is_dir = flag != NULL && Z_TYPE_P(flag) == IS_TRUE;
			}

			zval_ptr_dtor(&stat_result);
		} else if (refusal != NULL) {
			zend_string_release(refusal);
		} else {
			zval_ptr_dtor(&stat_args[0]);
			smart_str_free(&child);
			zval_ptr_dtor(&result);
			LUAEXT_NO_RAISE_END(L);
			return false;
		}

		zval_ptr_dtor(&stat_args[0]);

		if (is_dir) {
			if (!luaext_vfs_count_files(L, sandbox, ZSTR_VAL(child.s), ceiling, depth + 1, count)) {
				smart_str_free(&child);
				zval_ptr_dtor(&result);
				LUAEXT_NO_RAISE_END(L);
				return false;
			}
		} else {
			(*count)++;
		}

		smart_str_free(&child);
	}
	ZEND_HASH_FOREACH_END();

	zval_ptr_dtor(&result);

	LUAEXT_NO_RAISE_END(L);

	return true;
}

/*
 * Refuse a new file when the namespace is already at VfsQuota::$maxFiles.
 *
 * Fatal rather than a refusal a script may handle, like the other quotas: a
 * script that could catch this would create in a loop, and the bound exists to
 * cap what the host's store has to hold.
 */
static bool luaext_vfs_charge_new_file(lua_State *L, luaext_sandbox *sandbox)
{
	uint32_t cap = sandbox->policy.vfs_quota.max_files;

	if (cap == 0) {
		return true;
	}

	if (!sandbox->vfs_file_count_known) {
		uint32_t count = 0;

		if (!luaext_vfs_count_files(L, sandbox, "/", cap, 0, &count)) {
			return false;
		}

		sandbox->vfs_file_count = count;
		sandbox->vfs_file_count_known = true;
	}

	if (sandbox->vfs_file_count >= cap) {
		luaext_error_raise(L, LUAEXT_ERR_VFS, true,
						   "The filesystem already holds %u file(s), which is its "
						   "VfsQuota::$maxFiles",
						   (unsigned int)cap);
		return false;
	}

	sandbox->vfs_file_count++;

	return true;
}

bool luaext_vfs_open(lua_State *L, luaext_sandbox *sandbox, zend_string *path, const char *mode,
					 zend_string **refusal)
{
	luaext_vfs_handle *handle;
	uint32_t cap = sandbox->policy.vfs_quota.max_open_handles;
	bool readable;
	bool writable;
	bool append;
	bool truncate;
	bool exists = false;

	*refusal = NULL;

	if (!luaext_vfs_parse_mode(mode, &readable, &writable, &append, &truncate)) {
		luaext_error_raise(L, LUAEXT_ERR_VFS, false,
						   "\"%s\" is not a file mode; use r, w, a, r+, w+ or a+", mode);
		return false;
	}

	if (writable && !luaext_vfs_writable(sandbox)) {
		/*
		 * Catchable, unlike a quota. A script asking to write without the
		 * capability is asking for something the host chose not to grant, which
		 * is exactly the kind of refusal a script is meant to handle -- and
		 * unlike a quota, retrying cannot cost the host anything.
		 */
		*refusal = zend_string_init("the sandbox may not write files",
									strlen("the sandbox may not write files"), 0);
		return false;
	}

	if (cap != 0 && sandbox->vfs_open_handles >= cap) {
		luaext_error_raise(L, LUAEXT_ERR_VFS, true,
						   "The sandbox already has %u file(s) open, which is its "
						   "VfsQuota::$maxOpenHandles",
						   (unsigned int)cap);
		return false;
	}

	/*
	 * Existence decides two different things -- whether "r" fails and whether a
	 * buffered open has anything to load -- so it is asked once here rather than
	 * inferred from a later read's refusal.
	 */
	{
		zval args[1];
		zval result;

		ZVAL_STR(&args[0], path);

		if (luaext_vfs_call(L, sandbox, "exists", 1, args, &result, refusal) != LUAEXT_VFS_OK) {
			return false;
		}

		exists = Z_TYPE(result) == IS_TRUE;
		zval_ptr_dtor(&result);
	}

	if (!exists && !writable) {
		*refusal = zend_string_init("no such file", strlen("no such file"), 0);
		return false;
	}

	/* The one place a file comes into existence. Charged before the handle is
	 * built, so a refusal costs nothing to unwind. */
	if (!exists && !luaext_vfs_charge_new_file(L, sandbox)) {
		return false;
	}

	handle = (luaext_vfs_handle *)lua_newuserdatauv(L, sizeof(*handle), 0);
	memset(handle, 0, sizeof(*handle));

	handle->readable = readable;
	handle->writable = writable;
	handle->append = append;
	handle->ranged = sandbox->vfs_ranged;
	handle->path = zend_string_copy(path);

	lua_rawgetp(L, LUA_REGISTRYINDEX, &luaext_key_filemt);
	lua_setmetatable(L, -2);

	/*
	 * Counted BEFORE anything else can fail, and the registry entry goes in with
	 * it. Between those two steps a raised error would leave a handle that is
	 * counted but unreachable -- so nothing that can raise belongs in the middle.
	 */
	sandbox->vfs_open_handles++;

	luaext_vfs_push_handles(L);
	lua_pushvalue(L, -2);
	lua_pushboolean(L, 1);
	lua_rawset(L, -3);
	lua_pop(L, 1);

	if (handle->ranged) {
		/*
		 * Nothing to load. A ranged truncate is the backend's job and is issued
		 * here so "w" means the same thing to both kinds of backend.
		 */
		if (truncate && exists) {
			zval args[2];
			zval result;

			ZVAL_STR(&args[0], path);
			ZVAL_LONG(&args[1], 0);

			if (luaext_vfs_call(L, sandbox, "truncate", 2, args, &result, refusal) !=
				LUAEXT_VFS_OK) {
				luaext_vfs_handle_release(sandbox, handle);
				return false;
			}

			zval_ptr_dtor(&result);
		}

		if (append && exists) {
			zval args[1];
			zval result;

			ZVAL_STR(&args[0], path);

			if (luaext_vfs_call(L, sandbox, "stat", 1, args, &result, refusal) != LUAEXT_VFS_OK) {
				luaext_vfs_handle_release(sandbox, handle);
				return false;
			}

			if (Z_TYPE(result) == IS_OBJECT) {
				zval *size = zend_read_property(luaext_ce_file_stat, Z_OBJ(result), "size",
												strlen("size"), 1, NULL);

				if (size != NULL && Z_TYPE_P(size) == IS_LONG && Z_LVAL_P(size) > 0) {
					handle->offset = (uint64_t)Z_LVAL_P(size);
				}
			}

			zval_ptr_dtor(&result);
		}

		return true;
	}

	/* Buffered: load the whole file unless the mode says its old contents are
	 * about to be thrown away. */
	if (exists && !truncate) {
		zval args[1];
		zval result;

		ZVAL_STR(&args[0], path);

		if (luaext_vfs_call(L, sandbox, "read", 1, args, &result, refusal) != LUAEXT_VFS_OK) {
			luaext_vfs_handle_release(sandbox, handle);
			return false;
		}

		if (Z_TYPE(result) != IS_STRING) {
			zval_ptr_dtor(&result);
			luaext_vfs_handle_release(sandbox, handle);
			luaext_error_raise(L, LUAEXT_ERR_VFS, false, "%s",
							   "FileSystem::read() did not return a string");
			return false;
		}

		/*
		 * Ownership first, then the zval goes, then the charge -- which raises.
		 * Charging while still holding `result` leaked it on every refusal,
		 * since the raise longjmps past the dtor.
		 */
		luaext_vfs_note_bytes(sandbox, Z_STRLEN(result));
		handle->buffer = zend_string_copy(Z_STR(result));
		zval_ptr_dtor(&result);

		if (!luaext_vfs_charge_buffer_public(L, sandbox, ZSTR_LEN(handle->buffer))) {
			luaext_vfs_handle_release(sandbox, handle);
			return false;
		}
	} else {
		handle->buffer = zend_string_alloc(0, 0);
		ZSTR_LEN(handle->buffer) = 0;
		ZSTR_VAL(handle->buffer)[0] = '\0';

		/* A truncating open of an existing file is itself a change, or closing
		 * without writing would leave the old contents behind. */
		handle->dirty = truncate && exists;
	}

	if (append) {
		handle->offset = ZSTR_LEN(handle->buffer);
	}

	return true;
}

bool luaext_vfs_handle_close(lua_State *L, luaext_sandbox *sandbox, luaext_vfs_handle *handle,
							 zend_string **refusal)
{
	*refusal = NULL;

	if (handle->closed) {
		return true;
	}

	if (handle->dirty && handle->buffer != NULL) {
		zval args[2];
		zval result;

		ZVAL_STR(&args[0], handle->path);
		ZVAL_STR(&args[1], handle->buffer);

		if (luaext_vfs_call(L, sandbox, "write", 2, args, &result, refusal) != LUAEXT_VFS_OK) {
			/*
			 * Released even so. The bytes are gone either way -- there is nowhere
			 * else to put them -- and keeping the handle open would leak the
			 * buffer and the quota slot on every failed close.
			 */
			luaext_vfs_handle_release(sandbox, handle);
			return false;
		}

		luaext_vfs_note_bytes(sandbox, ZSTR_LEN(handle->buffer));
		zval_ptr_dtor(&result);
		handle->dirty = false;
	}

	luaext_vfs_handle_release(sandbox, handle);

	return true;
}

void luaext_vfs_sweep(luaext_sandbox *sandbox)
{
	lua_State *L = sandbox->L;

	if (L == NULL || sandbox->vfs_open_handles == 0) {
		return;
	}

	luaext_vfs_push_handles(L);

	lua_pushnil(L);

	while (lua_next(L, -2) != 0) {
		luaext_vfs_handle *handle = (luaext_vfs_handle *)lua_touserdata(L, -2);

		lua_pop(L, 1); /* value; the key stays for lua_next */

		if (handle != NULL && !handle->closed) {
			zend_string *refusal = NULL;

			/*
			 * A failure here cannot be reported: the call is returning and no
			 * script is left to hear it. What must NOT happen is a pending PHP
			 * exception escaping into the caller's teardown, so it is cleared --
			 * the flush was best-effort by the time we reached the sweep.
			 */
			(void)luaext_vfs_handle_close(L, sandbox, handle, &refusal);

			if (refusal != NULL) {
				zend_string_release(refusal);
			}

			if (EG(exception)) {
				zend_clear_exception();
			}
		}
	}

	lua_pop(L, 1);

	/* The table is rebuilt empty rather than drained key by key: every handle in
	 * it is closed now, and clearing during the walk above would be a mutation
	 * lua_next does not allow. */
	lua_createtable(L, 0, 8);
	lua_rawsetp(L, LUA_REGISTRYINDEX, &luaext_key_handles);

	sandbox->vfs_open_handles = 0;
}
