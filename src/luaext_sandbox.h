/*
 * luaext — the Sandbox object.
 *
 * Only the handful of entry points the module needs; everything else about a
 * sandbox is private to luaext_sandbox.c. The data layout itself lives in
 * luaext_types.h.
 */

#ifndef LUAEXT_SANDBOX_H
#define LUAEXT_SANDBOX_H

#include "luaext_types.h"

/*
 * Install the Sandbox object handlers. Called from MINIT once
 * luaext_ce_sandbox exists.
 */
void luaext_sandbox_startup(void);

/*
 * Release the interpreter and drop the sandbox from the per-thread live list.
 * Idempotent, and safe on a sandbox whose construction failed part way.
 * Called by close(), by the object destructor and by the RSHUTDOWN sweep.
 */
void luaext_sandbox_close(luaext_sandbox *sandbox);

#endif /* LUAEXT_SANDBOX_H */
