/*
 * luaext — the bridge between a script's file operations and the host's
 * FileSystem.
 *
 * Three responsibilities, and they are separated because they fail differently:
 *
 *   luaext_vfs_path.c  turns a script's string into a canonical name. Pure, so
 *                      it can be fuzzed; see its header.
 *   THIS FILE          decides whether an operation is allowed, charges it, and
 *                      calls the host. Everything a script can influence about
 *                      SIZE and POSITION is validated here.
 *   luaext_iolib.c     is the Lua surface: io.open, handle methods, io.lines.
 *
 * THE ARITHMETIC IS THE SECURITY BOUNDARY. Offsets and lengths arrive as Lua
 * integers, which a script chooses freely, and they are 64-bit signed. Three
 * things follow, none of them optional:
 *
 *   - A negative offset or length is refused. Passed onward it becomes a huge
 *     size_t somewhere below.
 *   - offset + length is checked for overflow BEFORE it is used. Wrapping turns
 *     a colossal range into a small one that passes every later test.
 *   - A range write is judged on the END position, not on how many bytes it
 *     carries. A one-byte write at offset 2^40 asks the backend for a one
 *     terabyte file, and the per-file quota has to see it as one.
 *
 * ERRORS SPLIT IN TWO, and the split is an allowlist rather than a catch-all.
 * A VfsError from the backend is the script's business: it comes back as
 * `nil, message` the way any io error does. ANY other exception is the host's
 * failure -- a database down, a bug in the backend -- and is fatal, preserved
 * and rethrown to the caller intact. Treating those as script-visible would let
 * a script probe for backend outages and swallow them.
 */

#ifndef LUAEXT_VFS_H
#define LUAEXT_VFS_H

#include "luaext_types.h"

/* What a backend call produced. */
typedef enum {
	LUAEXT_VFS_OK = 0,

	/*
	 * The operation failed in a way a script may see and handle: not found, a
	 * quota, a refusal the backend expressed as VfsError. The caller turns this
	 * into `nil, message, code`.
	 */
	LUAEXT_VFS_REFUSED,

	/*
	 * The host failed, or the script must stop. A PHP exception is pending, or
	 * a Lua error has been raised. The caller must not convert this into a
	 * return value.
	 */
	LUAEXT_VFS_FAILED,
} luaext_vfs_result;

/*
 * Attach the configured backend, if the vfs capability was granted.
 *
 * Takes its own reference to the FileSystem object rather than reaching back
 * through sandbox->config_zv on every call, for the reason the output sink
 * does: a subsystem that owns what it calls does not depend on teardown
 * ordering staying true.
 */
bool luaext_vfs_init(luaext_sandbox *sandbox, zval *filesystem);

/* Convenience wrapper reading SandboxConfig::$filesystem. */
bool luaext_vfs_init_from_config(luaext_sandbox *sandbox, zval *config);

/* Release the backend and close every handle still open. */
void luaext_vfs_shutdown(luaext_sandbox *sandbox);

/* Whether this sandbox has a filesystem at all. */
bool luaext_vfs_available(const luaext_sandbox *sandbox);

/* Whether writes are permitted, i.e. the vfsWrite capability was granted. */
bool luaext_vfs_writable(const luaext_sandbox *sandbox);

/*
 * Reset the per-call operation counter.
 *
 * VfsQuota::$maxOperations bounds calls into the backend PER SANDBOX CALL, not
 * for the sandbox's life: a host that runs many calls should not find the
 * hundredth refused because the first ninety-nine spent the budget.
 */
void luaext_vfs_begin_call(luaext_sandbox *sandbox);

/*
 * Charge one backend operation. Returns false with a Lua error raised when the
 * per-call budget is spent -- fatal, because a script able to catch it would
 * simply retry.
 */
bool luaext_vfs_charge_operation(lua_State *L, luaext_sandbox *sandbox);

/*
 * Validate a script-supplied offset/length pair against the per-file quota.
 *
 * `length` may be zero for a pure seek. On success `*end_position` holds
 * offset + length, already known not to have overflowed. See the header comment
 * on why the end position rather than the length is what the quota judges.
 */
/*
 * Call one FileSystem method, splitting the two error classes.
 *
 * On LUAEXT_VFS_REFUSED, `*refusal` holds the VfsError's message for the caller
 * to hand back as `nil, message`; the caller owns and must release it. On
 * LUAEXT_VFS_FAILED a PHP exception is pending or a Lua error was raised, and
 * the caller must return that outward rather than convert it.
 */
luaext_vfs_result luaext_vfs_call(lua_State *L, luaext_sandbox *sandbox, const char *method,
								  uint32_t argc, zval *args, zval *result, zend_string **refusal);

bool luaext_vfs_check_range(lua_State *L, const luaext_sandbox *sandbox, lua_Integer offset,
							lua_Integer length, uint64_t *end_position);

/* -------------------------------------------------------------------------
 * Open files
 *
 * A handle is a Lua userdata carrying this struct, tracked in a STRONG registry
 * table for as long as it is open. Strong is the point: a script that opens a
 * file and drops the reference still has it open, and a weak table would let
 * the collector decide when VfsQuota::$maxOpenHandles is satisfied.
 *
 * Two backing strategies, chosen once at construction from whether the backend
 * implements RangedFileSystem:
 *
 *   RANGED    -- nothing is held here. Reads and writes become readRange and
 *                writeRange at the handle's offset.
 *   BUFFERED  -- the whole file is read once into `buffer` and written back on
 *                flush. The only option when the backend can only move whole
 *                files, and the reason maxTotalBytes exists: those buffers are
 *                the sandbox's memory, and they are billed as such.
 * ---------------------------------------------------------------------- */

typedef struct {
	zend_string *path; /* canonical, owned */

	/*
	 * Whole-file contents for a buffered handle, NULL for a ranged one. Owned,
	 * and its length is what this handle contributes to vfs_buffered_bytes.
	 */
	zend_string *buffer;

	uint64_t offset; /* read/write position */

	bool readable;
	bool writable;
	bool append; /* every write goes to the end, whatever the offset */
	bool ranged; /* streams rather than buffers */
	bool dirty;	 /* buffer differs from what the backend holds */
	bool closed;
} luaext_vfs_handle;

/*
 * Push a new handle userdata for `path`, already canonicalised.
 *
 * `mode` is Lua's: r, w, a, r+, w+, a+, with an optional b that is accepted and
 * ignored because there is no text/binary distinction to make here. Returns
 * false with `*refusal` set when the backend refused in a way the script may
 * see, and false with a Lua error raised or a PHP exception pending otherwise --
 * the caller must tell those apart by checking `*refusal`.
 */
bool luaext_vfs_open(lua_State *L, luaext_sandbox *sandbox, zend_string *path, const char *mode,
					 zend_string **refusal);

/*
 * Write a dirty buffer back and mark the handle closed, releasing what it owns.
 *
 * Safe to call twice. Returns false when the backend refused, with `*refusal`
 * set, or when it failed -- the same split as luaext_vfs_call().
 */
bool luaext_vfs_handle_close(lua_State *L, luaext_sandbox *sandbox, luaext_vfs_handle *handle,
							 zend_string **refusal);

/*
 * Canonicalise the Lua string at `index` into a path the backend may see.
 *
 * Shared by io and os so there is exactly one place a script's string becomes a
 * name the host is handed. Returns NULL with a Lua error raised; the caller owns
 * the result and must release it.
 */
zend_string *luaext_vfs_path_from_lua(lua_State *L, luaext_sandbox *sandbox, int index);

/*
 * Charge `bytes` of new buffering against VfsQuota::$maxTotalBytes.
 *
 * Called by the write path when a buffered handle grows. Raises a fatal Lua
 * error and returns false when the budget is spent -- fatal because a script
 * able to catch it would keep writing.
 */
bool luaext_vfs_charge_buffer_public(lua_State *L, luaext_sandbox *sandbox, size_t bytes);

/*
 * Release what a handle owns, WITHOUT calling the backend.
 *
 * This is what a handle's __gc may do and all it may do: reaching the host from
 * inside Lua's collector is the hazard luaext_defer.h describes. Anything that
 * needs the backend has to have happened at the sweep already.
 */
void luaext_vfs_handle_gc(luaext_sandbox *sandbox, luaext_vfs_handle *handle);

/*
 * Close every handle still open, at the end of the outermost call.
 *
 * Runs at the same point as the coroutine sweep and for the same reason: a file
 * left open is state that must not outlive the call that made it. Errors are
 * swallowed here -- the call is already returning, and a backend that refuses a
 * flush at teardown cannot be reported to a script that has stopped running.
 */
void luaext_vfs_sweep(luaext_sandbox *sandbox);

#endif /* LUAEXT_VFS_H */
