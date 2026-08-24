/*
 * luaext — the math library a sandbox sees.
 *
 * SCAFFOLD. Nothing is implemented yet.
 *
 * Two jobs, and the second is the one that is easy to overlook:
 *
 *   Replace math.randomseed so it returns nothing. Upstream 5.4+ returns the
 *   seed components it derived, and those are partly address-based -- a
 *   straight ASLR disclosure.
 *
 *   RESEED the generator at install time from the sandbox's own seed. Upstream
 *   seeds it when the library opens, from a stack address mixed with a
 *   second-granularity clock, so a script that calls math.random(0) a few times
 *   can brute-force the state offline. Replacing the function does nothing
 *   about that; only reseeding does.
 */

#include "luaext_openlibs.h"

#include <lauxlib.h>
#include <lualib.h>
