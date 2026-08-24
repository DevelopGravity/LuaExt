/*
 * luaext — the os library a sandbox sees.
 *
 * SCAFFOLD. Publishes nothing yet, preserving today's behaviour (no os table).
 *
 * This library is BUILT, not filtered: loslib.c is never compiled, so
 * system(), popen() and tmpnam() are absent from the binary rather than merely
 * unreachable, and there is no upstream opener to select from.
 *
 * Scope for this wave is time and environment only -- os.time, os.date,
 * os.difftime, os.getenv, os.clock. os.remove and os.rename belong to the VFS
 * and arrive with it.
 *
 * os.clock returns the sandbox's OWN billed CPU, via luaext_timers_cpu_seconds,
 * rounded to a coarse grid. Rounding raises the cost of using it as a timing
 * oracle by forcing an attacker to average over many samples; it does not
 * eliminate one, and the honest reason it is safe enough is that it measures
 * only this sandbox's own budget and there is nothing else in the process a
 * script can name. It must never return a constant: a frozen clock surfaces as
 * mysterious script bugs rather than as a missing feature.
 */

#include "luaext_openlibs.h"

#include <lauxlib.h>
#include <lualib.h>

bool luaext_oslib_install(lua_State *L, luaext_sandbox *sandbox)
{
	(void)L;
	(void)sandbox;
	return true;
}
