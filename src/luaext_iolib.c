/*
 * luaext — the `io` table.
 *
 * It has two halves that answer to different capabilities, and conflating them
 * is the mistake this file is arranged to avoid.
 *
 *   THE OUTPUT HALF -- io.write, io.stdout, io.stderr -- is always present.
 *   These are the sandbox's stdout and stderr, and they go to the configured
 *   output sink like print() does. They need no filesystem: writing a partial
 *   line is not a storage operation, and an earlier draft of the docs required
 *   the vfs capability for them, which would have meant a script could not
 *   write without also being handed a backend.
 *
 *   THE FILESYSTEM HALF -- io.open, io.lines, io.close -- needs the vfs
 *   capability and a FileSystem behind it. Everything it does goes through
 *   luaext_vfs.c, which validates and charges before the host is called.
 *
 * There is deliberately no io.read and no io.stdin. A sandbox has no console,
 * and inventing one that always returns nil would be a surface that looks like
 * it might one day do something.
 */

#include "luaext_iolib.h"

#include "luaext_error.h"
#include "luaext_output.h"
#include "luaext_vfs.h"
#include "luaext_vfs_path.h"

#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>

/* -------------------------------------------------------------------------
 * Writing to the sink
 * ---------------------------------------------------------------------- */

/*
 * Shared by io.write and the two stream objects' :write.
 *
 * Accepts strings and numbers, the way upstream's does, and refuses everything
 * else by name rather than stringifying it -- io.write(t) silently emitting
 * "table: 0x..." would leak a heap address, which the vendored tostring patch
 * exists to prevent elsewhere.
 */
static int luaext_iolib_write_values(lua_State *L, int first, bool is_stderr)
{
	luaext_sandbox *sandbox = LUAEXT_SB(L);
	int top = lua_gettop(L);
	int index;

	for (index = first; index <= top; index++) {
		const char *text;
		size_t length;

		if (lua_type(L, index) == LUA_TNUMBER) {
			/* Converted in place, which is what upstream does: the number
			 * becomes its string form on the stack and stays anchored. */
			text = lua_tolstring(L, index, &length);
		} else if (lua_type(L, index) == LUA_TSTRING) {
			text = lua_tolstring(L, index, &length);
		} else {
			return luaL_argerror(
				L, index,
				lua_pushfstring(L, "string or number expected, got %s", luaL_typename(L, index)));
		}

		if (!luaext_output_write_channel(sandbox, text, length, is_stderr)) {
			/*
			 * Fatal, exactly as print()'s overflow is. A script that could catch
			 * its own output limit would write again, so OutputLimitError is one
			 * of the errors pcall must not swallow.
			 */
			luaext_error_raise(L, LUAEXT_ERR_OUTPUT, true,
							   "The sandbox has written all the output it is allowed");
		}
	}

	return 0;
}

static int luaext_iolib_write(lua_State *L)
{
	luaext_iolib_write_values(L, 1, false);

	/* Upstream returns the file so writes can be chained. io.write has no file
	 * to return here, so it returns the io table itself, which is what a chain
	 * would go on to use. */
	lua_pushvalue(L, lua_upvalueindex(1));

	return 1;
}

/* The stream objects. Upvalue 1 says which channel; argument 1 is self. */
static int luaext_iolib_stream_write(lua_State *L)
{
	bool is_stderr = lua_toboolean(L, lua_upvalueindex(1)) != 0;

	luaext_iolib_write_values(L, 2, is_stderr);

	lua_pushvalue(L, 1);

	return 1;
}

/*
 * A no-op that reports success.
 *
 * stdout and stderr are the sandbox's own sink, not files, and closing them
 * would mean a script could silence its own output and leave the host reading
 * an empty buffer wondering what happened. Upstream refuses to close the
 * standard streams too, so this matches rather than invents.
 */
static int luaext_iolib_stream_close(lua_State *L)
{
	lua_pushnil(L);
	lua_pushliteral(L, "cannot close a standard file");

	return 2;
}

static int luaext_iolib_stream_tostring(lua_State *L)
{
	bool is_stderr = lua_toboolean(L, lua_upvalueindex(1)) != 0;

	lua_pushstring(L, is_stderr ? "file (stderr)" : "file (stdout)");

	return 1;
}

/* Build io.stdout or io.stderr: a table with :write, :close and __tostring. */
static void luaext_iolib_push_stream(lua_State *L, bool is_stderr)
{
	lua_createtable(L, 0, 2);

	lua_pushboolean(L, is_stderr);
	lua_pushcclosure(L, luaext_iolib_stream_write, 1);
	lua_setfield(L, -2, "write");

	lua_pushcfunction(L, luaext_iolib_stream_close);
	lua_setfield(L, -2, "close");

	/* Its own metatable, so __tostring names the stream instead of printing the
	 * table's address -- the same disclosure the vendored tostring patch closes
	 * for every other value. */
	lua_createtable(L, 0, 2);

	lua_pushboolean(L, is_stderr);
	lua_pushcclosure(L, luaext_iolib_stream_tostring, 1);
	lua_setfield(L, -2, "__tostring");

	lua_pushboolean(L, 0);
	lua_setfield(L, -2, "__metatable");

	lua_setmetatable(L, -2);
}

/* -------------------------------------------------------------------------
 * File handles
 *
 * Everything below exists only when the vfs capability is granted. The rules it
 * enforces live in luaext_vfs.c; this half is the Lua surface over them.
 * ---------------------------------------------------------------------- */

/*
 * The path a backend FAILURE takes, as opposed to a refusal.
 *
 * luaext_vfs_call leaves a host exception PENDING rather than converting it, so
 * that the class the backend threw is what reaches the caller. Raising a bare
 * lua_error here instead would push whatever happens to be on the stack -- in
 * practice the half-built handle -- and the boundary would report that as a
 * RuntimeError naming a file, burying the database outage underneath it.
 */
static int luaext_iolib_failed(lua_State *L)
{
	if (EG(exception) != NULL) {
		/* Retains the object, decides catchable from its class, does not return. */
		luaext_error_raise_from_exception(L);
	}

	/* No exception means luaext_error_raise already ran and did not return, so
	 * this is unreachable; it keeps the stack value meaningful if that changes. */
	return lua_error(L);
}

/* `nil, message` -- the conventional Lua shape for a refusal a script may
 * handle. Owns and releases the refusal string. */
static int luaext_iolib_refused(lua_State *L, zend_string *refusal)
{
	lua_pushnil(L);

	if (refusal != NULL) {
		lua_pushlstring(L, ZSTR_VAL(refusal), ZSTR_LEN(refusal));
		zend_string_release(refusal);
	} else {
		lua_pushliteral(L, "the filesystem refused the operation");
	}

	return 2;
}

static luaext_vfs_handle *luaext_iolib_check_handle(lua_State *L, int index)
{
	luaext_vfs_handle *handle =
		(luaext_vfs_handle *)luaL_checkudata(L, index, LUAEXT_IOLIB_FILE_MT);

	if (handle->closed) {
		luaL_error(L, "attempt to use a closed file");
	}

	return handle;
}

/*
 * Read `length` bytes at the handle's offset, pushing a string.
 *
 * The two backing strategies converge here: a buffered handle slices what it
 * already holds, a ranged one asks the backend. Returns -1 on failure with a
 * Lua error raised or `*refusal` set.
 */
static int luaext_iolib_read_bytes(lua_State *L, luaext_sandbox *sandbox, luaext_vfs_handle *handle,
								   uint64_t length, zend_string **refusal)
{
	*refusal = NULL;

	if (!handle->ranged) {
		size_t available = ZSTR_LEN(handle->buffer);
		size_t start = handle->offset > available ? available : (size_t)handle->offset;
		size_t take = available - start;

		if ((uint64_t)take > length) {
			take = (size_t)length;
		}

		lua_pushlstring(L, ZSTR_VAL(handle->buffer) + start, take);
		handle->offset = start + take;

		return (int)take;
	}

	{
		zval args[3];
		zval result;
		int produced;

		ZVAL_STR(&args[0], handle->path);
		ZVAL_LONG(&args[1], (zend_long)handle->offset);
		ZVAL_LONG(&args[2], (zend_long)length);

		if (luaext_vfs_call(L, sandbox, "readRange", 3, args, &result, refusal) != LUAEXT_VFS_OK) {
			return -1;
		}

		if (Z_TYPE(result) != IS_STRING) {
			zval_ptr_dtor(&result);
			luaext_error_raise(L, LUAEXT_ERR_VFS, false, "%s",
							   "RangedFileSystem::readRange() did not return a string");
			return -1;
		}

		produced = (int)Z_STRLEN(result);
		luaext_vfs_note_bytes(sandbox, (size_t)produced);
		lua_pushlstring(L, Z_STRVAL(result), Z_STRLEN(result));
		handle->offset += (uint64_t)produced;
		zval_ptr_dtor(&result);

		return produced;
	}
}

/*
 * Read up to and including the next newline.
 *
 * Ranged backends are read in chunks rather than a byte at a time: a line read
 * that cost one backend call per character would spend a script's whole
 * VfsQuota::$maxOperations on a single line.
 */
#define LUAEXT_IOLIB_LINE_CHUNK 512

static int luaext_iolib_read_line(lua_State *L, luaext_sandbox *sandbox, luaext_vfs_handle *handle,
								  bool keep_newline, zend_string **refusal)
{
	luaL_Buffer line;
	bool any = false;

	*refusal = NULL;

	if (!handle->ranged) {
		size_t available = ZSTR_LEN(handle->buffer);
		size_t start = handle->offset > available ? available : (size_t)handle->offset;
		const char *base = ZSTR_VAL(handle->buffer);
		const char *newline;
		size_t take;

		if (start >= available) {
			return 0;
		}

		newline = memchr(base + start, '\n', available - start);
		take = newline != NULL ? (size_t)(newline - (base + start)) + 1 : available - start;

		handle->offset = start + take;

		if (!keep_newline && newline != NULL) {
			take--;
		}

		lua_pushlstring(L, base + start, take);

		return 1;
	}

	luaL_buffinit(L, &line);

	for (;;) {
		const char *chunk;
		size_t chunk_len;
		const char *newline;

		if (luaext_iolib_read_bytes(L, sandbox, handle, LUAEXT_IOLIB_LINE_CHUNK, refusal) < 0) {
			return -1;
		}

		chunk = lua_tolstring(L, -1, &chunk_len);

		if (chunk_len == 0) {
			lua_pop(L, 1);
			break;
		}

		any = true;
		newline = memchr(chunk, '\n', chunk_len);

		if (newline != NULL) {
			size_t upto = (size_t)(newline - chunk) + 1;

			/* Read past the line; hand the rest back by rewinding, since a
			 * ranged handle has nowhere to keep it. */
			handle->offset -= (uint64_t)(chunk_len - upto);

			luaL_addlstring(&line, chunk, keep_newline ? upto : upto - 1);
			lua_pop(L, 1);
			break;
		}

		luaL_addlstring(&line, chunk, chunk_len);
		lua_pop(L, 1);
	}

	luaL_pushresult(&line);

	if (!any) {
		lua_pop(L, 1);
		return 0;
	}

	return 1;
}

/* One :read format. Pushes exactly one value (possibly nil) and returns 1, or
 * returns -1 when the backend failed. */
static int luaext_iolib_read_one(lua_State *L, luaext_sandbox *sandbox, luaext_vfs_handle *handle,
								 int index, zend_string **refusal)
{
	*refusal = NULL;

	if (lua_type(L, index) == LUA_TNUMBER) {
		lua_Integer wanted = luaL_checkinteger(L, index);
		uint64_t end;

		if (!luaext_vfs_check_range(L, sandbox, (lua_Integer)handle->offset, wanted, &end)) {
			return -1;
		}

		if (luaext_iolib_read_bytes(L, sandbox, handle, (uint64_t)wanted, refusal) < 0) {
			return -1;
		}

		/* Upstream's rule: a zero-length result at end of file is nil, not "". */
		if (lua_rawlen(L, -1) == 0 && wanted > 0) {
			lua_pop(L, 1);
			lua_pushnil(L);
		}

		return 1;
	}

	{
		const char *format = luaL_checkstring(L, index);

		/* "*l" and "l" both, since 5.3 made the star optional and scripts in the
		 * wild still carry it. */
		if (format[0] == '*') {
			format++;
		}

		switch (format[0]) {
		case 'l':
		case 'L': {
			int got = luaext_iolib_read_line(L, sandbox, handle, format[0] == 'L', refusal);

			if (got < 0) {
				return -1;
			}

			if (got == 0) {
				lua_pushnil(L);
			}

			return 1;
		}

		case 'a': {
			/*
			 * Bounded by the per-file quota rather than by whatever the backend
			 * feels like returning: "read everything" is the one format where a
			 * script names no size, so the size has to come from the quota.
			 */
			size_t cap = sandbox->policy.vfs_quota.max_file_bytes;
			uint64_t wanted = cap != 0 ? (uint64_t)cap : UINT64_MAX;

			if (luaext_iolib_read_bytes(L, sandbox, handle, wanted, refusal) < 0) {
				return -1;
			}

			/* "a" answers with "" at end of file, never nil. */
			return 1;
		}

		case 'n':
			/*
			 * Refused rather than approximated. Reading a number means consuming
			 * exactly the characters that form one and no more, and a wrong
			 * answer here silently corrupts a script's data rather than failing.
			 */
			luaL_argerror(L, index,
						  "the \"n\" format is not supported; read bytes and use tonumber");
			return -1;

		default:
			luaL_argerror(L, index, lua_pushfstring(L, "invalid format \"%s\"", format));
			return -1;
		}
	}
}

static int luaext_iolib_file_read(lua_State *L)
{
	luaext_sandbox *sandbox = LUAEXT_SB(L);
	luaext_vfs_handle *handle = luaext_iolib_check_handle(L, 1);
	int top = lua_gettop(L);
	int index;
	int produced = 0;
	zend_string *refusal = NULL;

	if (!handle->readable) {
		return luaext_iolib_refused(L, zend_string_init("the file is not open for reading",
														strlen("the file is not open for reading"),
														0));
	}

	if (top == 1) {
		/* No format: one line without its newline, as upstream defaults. */
		int got = luaext_iolib_read_line(L, sandbox, handle, false, &refusal);

		if (got < 0) {
			return refusal != NULL ? luaext_iolib_refused(L, refusal) : luaext_iolib_failed(L);
		}

		if (got == 0) {
			lua_pushnil(L);
		}

		return 1;
	}

	luaL_checkstack(L, top, "luaext: too many read formats");

	for (index = 2; index <= top; index++) {
		if (luaext_iolib_read_one(L, sandbox, handle, index, &refusal) < 0) {
			return refusal != NULL ? luaext_iolib_refused(L, refusal) : luaext_iolib_failed(L);
		}

		produced++;

		/* Upstream stops at the first format that yields nil. */
		if (lua_isnil(L, -1)) {
			break;
		}
	}

	return produced;
}

static int luaext_iolib_file_write(lua_State *L)
{
	luaext_sandbox *sandbox = LUAEXT_SB(L);
	luaext_vfs_handle *handle = luaext_iolib_check_handle(L, 1);
	int top = lua_gettop(L);
	int index;

	if (!handle->writable) {
		return luaext_iolib_refused(L, zend_string_init("the file is not open for writing",
														strlen("the file is not open for writing"),
														0));
	}

	for (index = 2; index <= top; index++) {
		const char *data;
		size_t length;
		uint64_t end;

		if (lua_type(L, index) != LUA_TNUMBER && lua_type(L, index) != LUA_TSTRING) {
			return luaL_argerror(
				L, index,
				lua_pushfstring(L, "string or number expected, got %s", luaL_typename(L, index)));
		}

		data = lua_tolstring(L, index, &length);

		if (handle->append) {
			handle->offset = handle->ranged ? handle->offset : ZSTR_LEN(handle->buffer);
		}

		/*
		 * The quota is judged on where this write ENDS, not on how much it
		 * carries. A one-byte write at a huge offset asks the backend to hold a
		 * file that large, and luaext_vfs_check_range is where that is refused.
		 */
		if (!luaext_vfs_check_range(L, sandbox, (lua_Integer)handle->offset, (lua_Integer)length,
									&end)) {
			return lua_error(L);
		}

		if (handle->ranged) {
			zval args[3];
			zval result;
			zend_string *refusal = NULL;
			zend_string *payload = luaext_vfs_anchor_string(L, sandbox, data, length);

			if (payload == NULL) {
				return lua_error(L);
			}

			/*
			 * The payload is ANCHORED IN LUA, not owned here, and that is the
			 * whole point. A borrowed ZVAL_STR is not an option -- the bytes are
			 * Lua's and the backend may keep what it is handed -- so a copy has
			 * to exist. It used to be made with ZVAL_STRINGL and released on the
			 * next line, which never ran when luaext_vfs_call() raised: every
			 * write refused by the operations quota leaked its whole payload, at
			 * whatever size the script picked.
			 */
			ZVAL_STR(&args[0], handle->path);
			ZVAL_LONG(&args[1], (zend_long)handle->offset);
			ZVAL_STR(&args[2], payload);

			if (luaext_vfs_call(L, sandbox, "writeRange", 3, args, &result, &refusal) !=
				LUAEXT_VFS_OK) {
				return refusal != NULL ? luaext_iolib_refused(L, refusal) : luaext_iolib_failed(L);
			}

			luaext_vfs_note_bytes(sandbox, length);
			zval_ptr_dtor(&result);
		} else {
			/*
			 * Splice into the buffer. The write may land past the current end,
			 * in which case the gap is zero-filled the way a real file's is.
			 */
			size_t old_len = ZSTR_LEN(handle->buffer);
			size_t start = (size_t)handle->offset;
			size_t new_len = (size_t)end > old_len ? (size_t)end : old_len;
			zend_string *grown;

			if (new_len > old_len &&
				!luaext_vfs_charge_buffer_public(L, sandbox, new_len - old_len)) {
				return lua_error(L);
			}

			grown = zend_string_realloc(handle->buffer, new_len, 0);

			if (start > old_len) {
				memset(ZSTR_VAL(grown) + old_len, 0, start - old_len);
			}

			memcpy(ZSTR_VAL(grown) + start, data, length);
			ZSTR_VAL(grown)[new_len] = '\0';

			handle->buffer = grown;
			handle->dirty = true;
		}

		handle->offset = end;
	}

	lua_pushvalue(L, 1);

	return 1;
}

static int luaext_iolib_file_seek(lua_State *L)
{
	luaext_sandbox *sandbox = LUAEXT_SB(L);
	luaext_vfs_handle *handle = luaext_iolib_check_handle(L, 1);
	const char *whence = luaL_optstring(L, 2, "cur");
	lua_Integer offset = luaL_optinteger(L, 3, 0);
	int64_t base;
	int64_t target;

	if (strcmp(whence, "set") == 0) {
		base = 0;
	} else if (strcmp(whence, "cur") == 0) {
		base = (int64_t)handle->offset;
	} else if (strcmp(whence, "end") == 0) {
		if (handle->ranged) {
			zval args[1];
			zval result;
			zend_string *refusal = NULL;

			ZVAL_STR(&args[0], handle->path);

			if (luaext_vfs_call(L, sandbox, "stat", 1, args, &result, &refusal) != LUAEXT_VFS_OK) {
				return refusal != NULL ? luaext_iolib_refused(L, refusal) : luaext_iolib_failed(L);
			}

			base = 0;

			if (Z_TYPE(result) == IS_OBJECT) {
				zval *size = zend_read_property(luaext_ce_file_stat, Z_OBJ(result), "size",
												strlen("size"), 1, NULL);

				if (size != NULL && Z_TYPE_P(size) == IS_LONG) {
					base = (int64_t)Z_LVAL_P(size);
				}
			}

			zval_ptr_dtor(&result);
		} else {
			base = (int64_t)ZSTR_LEN(handle->buffer);
		}
	} else {
		return luaL_argerror(L, 2, lua_pushfstring(L, "invalid option \"%s\"", whence));
	}

	/*
	 * Checked before the addition is used. A script choosing both the base and
	 * the offset can name a pair that wraps, and a wrapped position would pass
	 * every quota test while describing somewhere else entirely.
	 */
	if ((offset > 0 && base > INT64_MAX - offset) || (offset < 0 && base < INT64_MIN - offset)) {
		luaext_error_raise(L, LUAEXT_ERR_VFS, false, "%s",
						   "That seek does not fit in a 64-bit file position");
		return lua_error(L);
	}

	target = base + (int64_t)offset;

	if (target < 0) {
		return luaext_iolib_refused(
			L, zend_string_init("cannot seek before the start of the file",
								strlen("cannot seek before the start of the file"), 0));
	}

	if (!luaext_vfs_check_range(L, sandbox, (lua_Integer)target, 0, NULL)) {
		return lua_error(L);
	}

	handle->offset = (uint64_t)target;
	lua_pushinteger(L, (lua_Integer)target);

	return 1;
}

static int luaext_iolib_file_close(lua_State *L)
{
	luaext_sandbox *sandbox = LUAEXT_SB(L);
	luaext_vfs_handle *handle = (luaext_vfs_handle *)luaL_checkudata(L, 1, LUAEXT_IOLIB_FILE_MT);
	zend_string *refusal = NULL;

	if (handle->closed) {
		lua_pushboolean(L, 1);
		return 1;
	}

	if (!luaext_vfs_handle_close(L, sandbox, handle, &refusal)) {
		return refusal != NULL ? luaext_iolib_refused(L, refusal) : luaext_iolib_failed(L);
	}

	/* Out of the open table, so the quota stops counting it immediately rather
	 * than at the next collection. */
	lua_rawgetp(L, LUA_REGISTRYINDEX, &luaext_key_handles);

	if (lua_type(L, -1) == LUA_TTABLE) {
		lua_pushvalue(L, 1);
		lua_pushnil(L);
		lua_rawset(L, -3);
	}

	lua_pop(L, 1);

	lua_pushboolean(L, 1);

	return 1;
}

static int luaext_iolib_file_flush(lua_State *L)
{
	luaext_sandbox *sandbox = LUAEXT_SB(L);
	luaext_vfs_handle *handle = luaext_iolib_check_handle(L, 1);

	if (handle->dirty && handle->buffer != NULL) {
		zval args[2];
		zval result;
		zend_string *refusal = NULL;

		ZVAL_STR(&args[0], handle->path);
		ZVAL_STR(&args[1], handle->buffer);

		if (luaext_vfs_call(L, sandbox, "write", 2, args, &result, &refusal) != LUAEXT_VFS_OK) {
			return refusal != NULL ? luaext_iolib_refused(L, refusal) : luaext_iolib_failed(L);
		}

		zval_ptr_dtor(&result);
		handle->dirty = false;
	}

	lua_pushvalue(L, 1);

	return 1;
}

/*
 * Releases memory only -- never the backend.
 *
 * A flush here would call PHP from inside Lua's collector, which is the hazard
 * luaext_defer.c exists for. It cannot be needed either: the call-scoped sweep
 * has already closed every handle by the time any of them can become garbage,
 * so a dirty buffer reaching this point would mean the sweep was skipped.
 */
static int luaext_iolib_file_gc(lua_State *L)
{
	luaext_vfs_handle *handle = (luaext_vfs_handle *)lua_touserdata(L, 1);
	luaext_sandbox *sandbox = LUAEXT_SB(L);

	if (handle != NULL && sandbox != NULL) {
		luaext_vfs_handle_gc(sandbox, handle);
	}

	return 0;
}

static int luaext_iolib_file_tostring(lua_State *L)
{
	luaext_vfs_handle *handle = (luaext_vfs_handle *)luaL_checkudata(L, 1, LUAEXT_IOLIB_FILE_MT);

	/* The virtual path, never a host path and never an address. */
	if (handle->closed) {
		lua_pushliteral(L, "file (closed)");
	} else {
		lua_pushfstring(L, "file (%s)", ZSTR_VAL(handle->path));
	}

	return 1;
}

/* -------------------------------------------------------------------------
 * io.open, io.lines, io.close
 * ---------------------------------------------------------------------- */

static int luaext_iolib_open(lua_State *L)
{
	luaext_sandbox *sandbox = LUAEXT_SB(L);
	const char *mode = luaL_optstring(L, 2, "r");
	zend_string *path;
	zend_string *refusal = NULL;

	path = luaext_vfs_path_from_lua(L, sandbox, 1);

	if (path == NULL) {
		return lua_error(L);
	}

	/*
	 * `path` is borrowed from a box on the stack, so this frame owns nothing
	 * across the open. It has to be that way: luaext_vfs_open() raises rather
	 * than returns for every quota it enforces, and a raise longjmps past
	 * whatever this frame is holding.
	 */
	if (!luaext_vfs_open(L, sandbox, path, mode, &refusal)) {
		return refusal != NULL ? luaext_iolib_refused(L, refusal) : luaext_iolib_failed(L);
	}

	return 1;
}

/*
 * Formats a single lines() iterator may carry.
 *
 * Each one becomes an upvalue and Lua caps a closure at 255 of those, so this
 * has to be below that with room for the three the iterator uses itself. It is
 * far past any real use: upstream's own examples pass one.
 */
#define LUAEXT_IOLIB_MAX_LINE_FORMATS 200

/*
 * The iterator io.lines and file:lines hand back.
 *
 * Upvalue 1 is the handle, 2 whether reaching the end should close it, 3 how
 * many formats follow, and 4.. the formats themselves.
 *
 * THE FORMATS USED TO BE MISSING ENTIRELY. Both lines() functions accepted them
 * -- Lua's signature is `io.lines(filename, ...)` -- and then read a plain line
 * whatever was asked for, so `f:lines("L")` silently dropped the newline it was
 * specifically asked to keep. Silently: no error, just the wrong bytes. They go
 * through luaext_iolib_read_one() now, which is the same function :read() uses,
 * so the two cannot answer differently again.
 */
static int luaext_iolib_lines_iter(lua_State *L)
{
	luaext_sandbox *sandbox = LUAEXT_SB(L);
	luaext_vfs_handle *handle = (luaext_vfs_handle *)lua_touserdata(L, lua_upvalueindex(1));
	bool close_at_end = lua_toboolean(L, lua_upvalueindex(2)) != 0;
	int formats = (int)lua_tointeger(L, lua_upvalueindex(3));
	zend_string *refusal = NULL;
	int produced = 0;
	int index;

	if (handle == NULL || handle->closed) {
		return 0;
	}

	/* The formats go back onto a cleared stack at 1..formats, so read_one()
	 * finds them where it expects an argument, and its results land above. */
	lua_settop(L, 0);
	luaL_checkstack(L, formats + 2, "too many formats for one lines() iterator");

	for (index = 0; index < formats; index++) {
		lua_pushvalue(L, lua_upvalueindex(4 + index));
	}

	for (index = 1; index <= formats; index++) {
		int got = luaext_iolib_read_one(L, sandbox, handle, index, &refusal);

		if (got < 0) {
			return refusal != NULL ? luaext_iolib_refused(L, refusal) : luaext_iolib_failed(L);
		}

		produced++;

		/* Stop at the first format that read nothing: the ones after it would
		 * be reading past the end of the file. */
		if (lua_isnil(L, -1)) {
			break;
		}
	}

	/* The FIRST result decides whether the loop goes round again, which is what
	 * makes `for line in f:lines("l", "l")` end on the pair that runs out. */
	if (!lua_isnil(L, formats + 1)) {
		return produced;
	}

	if (close_at_end) {
		(void)luaext_vfs_handle_close(L, sandbox, handle, &refusal);

		if (refusal != NULL) {
			zend_string_release(refusal);
		}
	}

	return 0;
}

/*
 * Build the iterator closure over a handle already on the stack top, taking the
 * formats from `first`..`last` of the caller's arguments.
 *
 * Shared so io.lines and file:lines cannot drift apart, which is how the format
 * argument came to be honoured by neither.
 */
static void luaext_iolib_push_lines_iter(lua_State *L, int handle_index, bool close_at_end,
										 int first, int last)
{
	int formats = last >= first ? last - first + 1 : 0;
	int index;

	luaL_argcheck(L, formats <= LUAEXT_IOLIB_MAX_LINE_FORMATS, first, "too many formats");
	luaL_checkstack(L, formats + 4, "too many formats for one lines() iterator");

	lua_pushvalue(L, handle_index);
	lua_pushboolean(L, close_at_end ? 1 : 0);

	/* No format means a line, the same default :read() applies. Materialised
	 * here rather than special-cased in the iterator, so the iterator has one
	 * shape for every call. */
	if (formats == 0) {
		lua_pushinteger(L, 1);
		lua_pushliteral(L, "l");
		lua_pushcclosure(L, luaext_iolib_lines_iter, 4);

		return;
	}

	lua_pushinteger(L, formats);

	for (index = first; index <= last; index++) {
		lua_pushvalue(L, index);
	}

	lua_pushcclosure(L, luaext_iolib_lines_iter, 3 + formats);
}

static int luaext_iolib_file_lines(lua_State *L)
{
	(void)luaext_iolib_check_handle(L, 1);

	/* the script opened it; the script closes it */
	luaext_iolib_push_lines_iter(L, 1, false, 2, lua_gettop(L));

	return 1;
}

static int luaext_iolib_lines(lua_State *L)
{
	luaext_sandbox *sandbox = LUAEXT_SB(L);
	zend_string *path;
	zend_string *refusal = NULL;

	/* Counted BEFORE anything else pushes: canonicalising leaves its path box on
	 * the stack and opening leaves the handle, so the caller's own arguments are
	 * only identifiable from here. */
	int last_format = lua_gettop(L);

	path = luaext_vfs_path_from_lua(L, sandbox, 1);

	if (path == NULL) {
		return lua_error(L);
	}

	if (!luaext_vfs_open(L, sandbox, path, "r", &refusal)) {
		/*
		 * Fatal, unlike io.open's nil. io.lines is used directly in a for
		 * clause, where a nil is not a value the loop can act on -- upstream
		 * raises here for the same reason.
		 *
		 * The message reads `path` AFTER the release that used to sit above it,
		 * which was a use-after-free on every refusal this branch exists to
		 * report. It is safe now for the same reason nothing is released here at
		 * all: the string belongs to a box on the stack, and the box outlives
		 * both this message and the unwind that carries it.
		 */
		if (refusal != NULL) {
			zend_string_release(refusal);
		}

		luaext_error_raise(L, LUAEXT_ERR_VFS, false, "Cannot open '%s' for reading",
						   ZSTR_VAL(path));
		return lua_error(L);
	}

	/* io.lines owns the handle, so the end of the file closes it. */
	luaext_iolib_push_lines_iter(L, lua_gettop(L), true, 2, last_format);

	return 1;
}

static int luaext_iolib_close(lua_State *L)
{
	if (lua_gettop(L) == 0) {
		/* No default output file to close: io.write goes to the sandbox's sink,
		 * which the host owns. */
		lua_pushboolean(L, 1);
		return 1;
	}

	return luaext_iolib_file_close(L);
}

/* Build the shared handle metatable once, into the registry. */
static void luaext_iolib_install_file_mt(lua_State *L)
{
	static const luaL_Reg methods[] = {
		{"read", luaext_iolib_file_read},
		{"write", luaext_iolib_file_write},
		{"seek", luaext_iolib_file_seek},
		{"close", luaext_iolib_file_close},
		{"flush", luaext_iolib_file_flush},
		{"lines", luaext_iolib_file_lines},
		{NULL, NULL},
	};

	luaL_newmetatable(L, LUAEXT_IOLIB_FILE_MT);

	lua_createtable(L, 0, 6);
	luaL_setfuncs(L, methods, 0);
	lua_setfield(L, -2, "__index");

	lua_pushcfunction(L, luaext_iolib_file_gc);
	lua_setfield(L, -2, "__gc");

	lua_pushcfunction(L, luaext_iolib_file_tostring);
	lua_setfield(L, -2, "__tostring");

	/* Unreachable from Lua, so a script cannot reach __gc or __index and cannot
	 * attach the metatable to a table of its own making. */
	lua_pushboolean(L, 0);
	lua_setfield(L, -2, "__metatable");

	/* Also keyed by address, which is how luaext_vfs_open finds it without
	 * paying for a string lookup on every open. */
	lua_pushvalue(L, -1);
	lua_rawsetp(L, LUA_REGISTRYINDEX, &luaext_key_filemt);

	lua_pop(L, 1);
}

/* -------------------------------------------------------------------------
 * Install
 * ---------------------------------------------------------------------- */

bool luaext_iolib_install(lua_State *L, luaext_sandbox *sandbox)
{
	luaL_checkstack(L, 8, "luaext: no stack to build the io library");

	lua_createtable(L, 0, 4);

	/*
	 * io.write needs the table itself as an upvalue so it can return it for
	 * chaining, which means the closure is built after the table exists and
	 * before the table is named.
	 */
	lua_pushvalue(L, -1);
	lua_pushcclosure(L, luaext_iolib_write, 1);
	lua_setfield(L, -2, "write");

	luaext_iolib_push_stream(L, false);
	lua_setfield(L, -2, "stdout");

	luaext_iolib_push_stream(L, true);
	lua_setfield(L, -2, "stderr");

	/*
	 * The filesystem half, only when there is a filesystem behind it.
	 *
	 * Absent rather than present-and-failing: a script can test for io.open to
	 * learn whether it has storage, which is the honest way to report a
	 * capability it does not have. A stub that returned `nil, "no filesystem"`
	 * would be indistinguishable from a backend that is merely down.
	 */
	if (luaext_vfs_available(sandbox)) {
		luaext_iolib_install_file_mt(L);

		lua_pushcfunction(L, luaext_iolib_open);
		lua_setfield(L, -2, "open");

		lua_pushcfunction(L, luaext_iolib_lines);
		lua_setfield(L, -2, "lines");

		lua_pushcfunction(L, luaext_iolib_close);
		lua_setfield(L, -2, "close");
	}

	lua_setglobal(L, LUA_IOLIBNAME);

	return true;
}
