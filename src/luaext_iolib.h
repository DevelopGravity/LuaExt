/*
 * luaext — the `io` table. See luaext_iolib.c for how its two halves divide.
 */

#ifndef LUAEXT_IOLIB_H
#define LUAEXT_IOLIB_H

#include "luaext_types.h"

/*
 * The handle metatable's name in the registry.
 *
 * Carries the extension's prefix so it cannot collide with a name any other
 * library registers, and luaL_checkudata compares against it to reject a
 * userdata from somewhere else being passed to a file method.
 */
#define LUAEXT_IOLIB_FILE_MT "luaext.file"

/*
 * Build the `io` table and set it as a global.
 *
 * Unconditional: the output half (io.write, io.stdout, io.stderr) is the
 * sandbox's own streams and needs no filesystem. The filesystem half is added
 * only when the vfs capability and a backend are both present.
 */
bool luaext_iolib_install(lua_State *L, luaext_sandbox *sandbox);

#endif /* LUAEXT_IOLIB_H */
