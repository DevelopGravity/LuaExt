/*
 * luaext — per-thread CPU clocks.
 *
 * SCAFFOLD. Reports "no CPU clock available", which is what makes
 * Sandbox::features() honestly say Unsupported today.
 *
 * PURITY RULE: no php.h, no lua.h. See luaext_watchdog.c.
 *
 * The three backends and their traps, for whoever implements this:
 *
 *   Linux/BSD  pthread_getcpuclockid() then clock_gettime(). No ownership.
 *   macOS      pthread_mach_thread_np(), NOT mach_thread_self() -- the latter
 *              returns a send right that must be mach_port_deallocate()'d, and
 *              the two are easy to confuse.
 *   Windows    DuplicateHandle() of GetCurrentThread(). That is a PSEUDO-handle
 *              meaning "the calling thread", so using it directly would measure
 *              the watchdog's own CPU instead of the sandbox owner's.
 */

#include "luaext_clock.h"

struct luaext_cpu_clock {
	int placeholder;
};

bool luaext_clock_capture_self(luaext_cpu_clock *out)
{
	(void)out;
	return false;
}

void luaext_clock_release(luaext_cpu_clock *clock)
{
	(void)clock;
}

bool luaext_clock_read(const luaext_cpu_clock *clock, uint64_t *ns)
{
	(void)clock;
	(void)ns;
	return false;
}

uint64_t luaext_clock_monotonic_ns(void)
{
	return 0;
}

uint64_t luaext_clock_cpu_resolution_ns(void)
{
	return 0;
}

bool luaext_clock_cpu_available(void)
{
	return false;
}
