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
	 * The filesystem half arrives with the VFS. Until then the table carries
	 * only the streams, and io.open is absent rather than present-and-failing:
	 * a script can test for it, which is the honest way to say a capability is
	 * not available. luaext_vfs_available() is what will gate it.
	 */
	(void)sandbox;

	lua_setglobal(L, LUA_IOLIBNAME);

	return true;
}
