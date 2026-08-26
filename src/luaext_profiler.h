/*
 * luaext — the sampling profiler.
 *
 * OFF BY DEFAULT, AND THAT IS THE DESIGN. Sampling needs the count hook, and any
 * non-zero hookmask sets ci->u.l.trap, which routes every instruction through
 * luaG_traceexec -- measured at 2.6x on dispatch-bound code. That cost is the
 * whole reason the interrupt check moved to the interpreter's back edges
 * (LUAEXT_VMCHECK). Arming a hook for a diagnostic that is off by default is
 * fine; arming one for a limit every sandbox has is not, which is why the shipped
 * hot path stays hook-free and this is opt-in.
 *
 * OWNER-SIDE, NEVER THE WATCHDOG. tools/check-watchdog-purity.sh forbids php.h
 * and lua.h anywhere the watchdog can reach, so the watchdog cannot walk a Lua
 * stack even in principle. Samples are taken by the hook, on the owner's thread,
 * where a lua_State is legal to touch.
 *
 * IT REFUSES RATHER THAN DISPLACING THE CPU LIMIT. When the watchdog thread
 * could not be started, the count hook stops being a fallback and becomes the
 * only thing that can notice a CPU overrun. Installing a profiling hook there
 * would silently remove the limit, so enableProfiler() returns false instead --
 * that is what its bool is for.
 *
 * THE HOOK RATE DOES NOT HAVE TO BE ACCURATE. A count hook fires every N
 * instructions, which is not a period; mapping seconds onto it needs a guess
 * about how fast this machine runs. That guess only sets RESOLUTION, never
 * correctness: Seconds and Percent are computed by scaling each function's share
 * of the samples by the CPU time actually measured, so a wrong rate produces
 * coarser sampling and still-correct totals.
 */

#ifndef LUAEXT_PROFILER_H
#define LUAEXT_PROFILER_H

#include "luaext_types.h"

/*
 * Arm the hook and start counting.
 *
 * Returns false, WITHOUT throwing, when profiling cannot be offered: the count
 * hook is currently carrying the CPU limit. A host asking to profile a sandbox
 * that would lose its limit gets an honest no.
 */
bool luaext_profiler_enable(luaext_sandbox *sandbox, double period_seconds);

/* Disarm the hook. Samples already taken are kept, so a host may profile a
 * region and read the result afterwards. */
void luaext_profiler_disable(luaext_sandbox *sandbox);

/* Whether sampling is currently armed. */
bool luaext_profiler_active(const luaext_sandbox *sandbox);

/*
 * Build the profile as a PHP array, most expensive first.
 *
 * `unit` is the ProfilerUnit case: 0 Samples, 1 Seconds, 2 Percent.
 */
void luaext_profiler_result(luaext_sandbox *sandbox, uint8_t unit, zval *out);

/* Release everything sampling owns. Safe on a sandbox that never profiled. */
void luaext_profiler_shutdown(luaext_sandbox *sandbox);

#endif /* LUAEXT_PROFILER_H */
