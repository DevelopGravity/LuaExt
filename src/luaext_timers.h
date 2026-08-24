/*
 * luaext — the PHP-facing half of the CPU and wall-clock limits.
 *
 * This is the impure layer: it may touch zvals, the sandbox and the
 * interpreter. The pure half lives in luaext_watchdog.c, which cannot include
 * php.h or lua.h and therefore cannot reach a zend_object, module globals or a
 * TSRM context even by accident. The split is the enforcement mechanism for
 * "nothing on the watchdog thread calls into PHP", not a stylistic preference.
 *
 * What actually stops a runaway script, in the order it fires:
 *
 *   1. An always-armed count hook on the interpreter. This is the correctness
 *      mechanism and the only thing that covers lvm.c's dispatch loop.
 *   2. The LUAEXT_CHECK sites patched into the vendored C library loops, which
 *      cover the long-running C functions no hook can interrupt.
 *   3. The PHP-callback boundary.
 *   4. A strided clock self-check inside the count hook.
 *
 * Tier 4 is why the CPU limit does not depend on the watchdog thread at all:
 * CPU can only be consumed where Lua instructions execute, so the thread that
 * is executing them can notice its own overrun. The watchdog thread is strictly
 * required only for the WALL limit while the owner is blocked OUTSIDE the VM --
 * a sleeping callback, a slow filesystem backend. Worth stating plainly,
 * because it means a failure to start the thread degrades one limit rather than
 * silently voiding both.
 *
 * There is deliberately no cross-thread lua_sethook. Upstream's "may be called
 * from a signal handler" means same-thread signal safety; from another thread it
 * writes L->hook and L->hookmask non-atomically and walks the CallInfo chain
 * that the owner is pushing and popping. It would be a data race, and it would
 * buy microseconds against a watchdog whose own resolution floor is coarser.
 */

#ifndef LUAEXT_TIMERS_H
#define LUAEXT_TIMERS_H

#include "luaext_types.h"

#include <lua.h>

/* -------------------------------------------------------------------------
 * Module lifecycle
 * ---------------------------------------------------------------------- */

/* MINIT: probe the clocks, initialise the slot pool and its locks. Does NOT
 * start the watchdog thread -- that is lazy, on the first armed limit. */
void luaext_timers_startup(void);

/* MSHUTDOWN: stop the thread, join it, then free the pool. In that order: the
 * join is what guarantees nothing is reading a slot as it is released. */
void luaext_timers_shutdown(void);

/* -------------------------------------------------------------------------
 * What Sandbox::features() reports
 *
 * These are PLATFORM statements, not per-sandbox ones. features() is static --
 * it has no sandbox and no configured limit -- so the rule "degrade when the
 * limit is close to the clock resolution" cannot live here. It belongs to
 * luaext_timers_set_cpu_limit(), which knows the limit.
 * ---------------------------------------------------------------------- */

luaext_limit_support luaext_timers_cpu_support(void);
luaext_limit_support luaext_timers_wall_support(void);
double luaext_timers_cpu_resolution_seconds(void);

/* -------------------------------------------------------------------------
 * Per-sandbox lifecycle. Owner thread only.
 * ---------------------------------------------------------------------- */

/* Take a slot and capture this thread's CPU clock. Called at construction, not
 * at first arm, so the clock is captured on the owning thread while it
 * demonstrably exists. */
bool luaext_timers_attach(luaext_sandbox *sandbox);

/* Release the slot and clear the interrupt flag. Idempotent, and safe on every
 * teardown path including the RSHUTDOWN sweep. Must run before lua_close():
 * finalisers running during teardown would otherwise see a pending interrupt. */
void luaext_timers_detach(luaext_sandbox *sandbox);

/* -------------------------------------------------------------------------
 * Limits. Nanoseconds; 0 lifts.
 * ---------------------------------------------------------------------- */

/*
 * Setting a limit does NOT reset what has already been consumed -- the new
 * budget is limit-minus-used.
 *
 * A deliberate divergence from the extension this replaces, which reset the
 * counter and so let a host callback call setCPULimit() in a loop and run
 * forever. It also keeps getCpuUsage() and the limit describing the same
 * quantity, which is what makes them comparable.
 *
 * Return false with a PHP exception already thrown when the limit cannot be
 * honoured. Accepting one silently and not enforcing it is the failure mode
 * this extension exists to eliminate.
 */
bool luaext_timers_set_cpu_limit(luaext_sandbox *sandbox, uint64_t ns);
bool luaext_timers_set_wall_limit(luaext_sandbox *sandbox, uint64_t ns);

/* -------------------------------------------------------------------------
 * The bracket around every entry into the interpreter
 * ---------------------------------------------------------------------- */

typedef struct {
	uint8_t was_paused;
	uint8_t prev_allow_pause;
} luaext_watch_frame;

/*
 * Only the OUTERMOST entry arms and disarms. A nested call made from inside a
 * PHP callback must not restart the clock or un-bill its caller's time.
 *
 * The nesting rule the reference implementation pins down: in a nested
 * PHP -> Lua -> PHP chain, any frame that did not explicitly pause makes the
 * whole chain count. That lives in sandbox->allow_pause, set here; the
 * accounting itself just bills whatever segments are open.
 */
void luaext_timers_enter_lua(luaext_sandbox *sandbox, luaext_watch_frame *frame);
void luaext_timers_leave_lua(luaext_sandbox *sandbox, const luaext_watch_frame *frame);

/* -------------------------------------------------------------------------
 * Pausing, for the PHP-callback boundary and the VFS bracket
 * ---------------------------------------------------------------------- */

#define LUAEXT_TIMER_CPU (1u << 0)
#define LUAEXT_TIMER_WALL (1u << 1)

bool luaext_timers_may_pause(const luaext_sandbox *sandbox);
bool luaext_timers_pause(luaext_sandbox *sandbox, uint8_t mask);
void luaext_timers_resume(luaext_sandbox *sandbox, uint8_t mask);

/* A callback that paused and forgot to resume does not get to keep the pause.
 * Called as a PHP callback returns. */
void luaext_timers_php_returned(luaext_sandbox *sandbox);

/* -------------------------------------------------------------------------
 * Usage. Owner thread; safe at any time, including mid-call.
 * ---------------------------------------------------------------------- */

/* Also what os.clock() returns: the sandbox's own billed CPU, so a script
 * timing itself measures exactly the quantity its limit enforces. */
double luaext_timers_cpu_seconds(const luaext_sandbox *sandbox);
double luaext_timers_wall_seconds(const luaext_sandbox *sandbox);

/* -------------------------------------------------------------------------
 * Asking for an interrupt without being able to raise one
 * ---------------------------------------------------------------------- */

/*
 * Sets the flag and returns. The next hook tick or LUAEXT_CHECK raises it.
 *
 * For callers holding unreleased zvals, where a lua_error() longjmp would leak
 * -- the output sink overflowing inside a frame that still owns its arguments,
 * and Sandbox::interrupt() from a foreign thread. A caller that CAN raise
 * should call luaext_error_raise() directly instead: one fewer tick of latency
 * and a traceback from the real failure point.
 *
 * Safe from any thread. Touches nothing but the two interrupt atomics.
 */
void luaext_timers_request(luaext_sandbox *sandbox, luaext_irq_reason reason);

/* The always-armed count hook. Installed at construction, never removed. */
void luaext_timers_hook(lua_State *L, lua_Debug *ar);

#endif /* LUAEXT_TIMERS_H */
