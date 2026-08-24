/*
 * luaext — the PHP-facing half of the CPU and wall-clock limits.
 *
 * The impure layer. Everything here may touch zvals, the sandbox and the
 * interpreter; nothing here runs on the watchdog thread. Its counterpart,
 * luaext_watchdog.c, cannot include php.h or lua.h and therefore cannot reach a
 * zend_object even by accident. See luaext_timers.h for the four tiers of
 * interrupt delivery and why tier 4 makes the CPU limit independent of the
 * watchdog thread.
 */

#include "luaext_timers.h"

#include "luaext_watchdog.h"

#include <Zend/zend_exceptions.h>

/*
 * The pure layer writes luaext_irq::reason as a bare number, because the enum
 * lives in a header it may not include. If these ever drift, a CPU breach
 * surfaces as a WallClockLimitError -- so they are checked here, where both
 * definitions are visible, rather than trusted.
 */
_Static_assert((unsigned)LUAEXT_IRQ_NONE == LUAEXT_WATCH_REASON_NONE,
			   "luaext_irq_reason and LUAEXT_WATCH_REASON_* must agree");
_Static_assert((unsigned)LUAEXT_IRQ_CPU == LUAEXT_WATCH_REASON_CPU,
			   "luaext_irq_reason and LUAEXT_WATCH_REASON_* must agree");
_Static_assert((unsigned)LUAEXT_IRQ_WALL == LUAEXT_WATCH_REASON_WALL,
			   "luaext_irq_reason and LUAEXT_WATCH_REASON_* must agree");

_Static_assert((unsigned)LUAEXT_TIMER_CPU == LUAEXT_WATCH_CPU,
			   "LUAEXT_TIMER_* and LUAEXT_WATCH_* must agree");
_Static_assert((unsigned)LUAEXT_TIMER_WALL == LUAEXT_WATCH_WALL,
			   "LUAEXT_TIMER_* and LUAEXT_WATCH_* must agree");

/*
 * A CPU clock this coarse or coarser makes every CPU limit a platform-level
 * approximation rather than an enforced one. One millisecond puts Linux (1 ns)
 * and macOS (1 us) on the Enforced side and Windows (~15.6 ms) on the Degraded
 * side, which is exactly the distinction Sandbox::features() exists to report.
 */
#define LUAEXT_TIMERS_FINE_NS UINT64_C(1000000)

/* Probed once at MINIT; every features() answer is derived from these. */
static bool luaext_timers_have_cpu = false;
static uint64_t luaext_timers_resolution_ns = 0;

/* -------------------------------------------------------------------------
 * Module lifecycle
 * ---------------------------------------------------------------------- */

void luaext_timers_startup(void)
{
	luaext_watchdog_startup();

	luaext_timers_have_cpu = luaext_clock_cpu_available();
	luaext_timers_resolution_ns = luaext_timers_have_cpu ? luaext_clock_cpu_resolution_ns() : 0;

	/*
	 * PHP_INI_SYSTEM, so one process-wide value is the whole story. Pushed down
	 * rather than pulled, because the watchdog cannot see an INI entry.
	 */
	luaext_watchdog_set_resolution_ns(
		(uint64_t)(LUAEXT_G(watchdog_resolution_us) > 0 ? LUAEXT_G(watchdog_resolution_us) : 0) *
		UINT64_C(1000));
}

void luaext_timers_shutdown(void)
{
	luaext_watchdog_shutdown();
}

/* -------------------------------------------------------------------------
 * What features() reports
 * ---------------------------------------------------------------------- */

/*
 * The count hook is the ONLY mechanism that can interrupt lvm.c's dispatch
 * loop. luaext.hook_count = 0 removes it, and with it every guarantee both
 * limits make about a script that never calls a patched C function. An INI that
 * silently voids a security guarantee is exactly what this extension exists to
 * prevent, so it is reported rather than absorbed.
 */
static bool luaext_timers_hook_armed(void)
{
	return LUAEXT_G(hook_count) > 0;
}

luaext_limit_support luaext_timers_cpu_support(void)
{
	if (!luaext_timers_hook_armed() || !luaext_timers_have_cpu) {
		return LUAEXT_LIMIT_UNSUPPORTED;
	}

	return luaext_timers_resolution_ns <= LUAEXT_TIMERS_FINE_NS ? LUAEXT_LIMIT_ENFORCED
																: LUAEXT_LIMIT_DEGRADED;
}

luaext_limit_support luaext_timers_wall_support(void)
{
	if (!luaext_timers_hook_armed()) {
		return LUAEXT_LIMIT_UNSUPPORTED;
	}

	/*
	 * Without the watchdog thread the wall limit still fires -- but only once
	 * Lua next executes an instruction, so a callback blocked in a slow host
	 * backend runs as long as it likes. That is a real degradation and saying so
	 * is the point of this method.
	 *
	 * Keyed on "was tried and refused", not on "is running": the thread is
	 * created lazily on the first armed limit, so a process that has not armed
	 * one yet has no thread and is not degraded. Reporting Degraded there would
	 * be wrong on every fresh process and would train hosts to ignore the field.
	 */
	return luaext_watchdog_thread_failed() ? LUAEXT_LIMIT_DEGRADED : LUAEXT_LIMIT_ENFORCED;
}

double luaext_timers_cpu_resolution_seconds(void)
{
	return (double)luaext_timers_resolution_ns / 1e9;
}

/* -------------------------------------------------------------------------
 * Per-sandbox lifecycle
 * ---------------------------------------------------------------------- */

bool luaext_timers_attach(luaext_sandbox *sandbox)
{
	atomic_store_explicit(&sandbox->irq.reason, (unsigned char)LUAEXT_IRQ_NONE,
						  memory_order_relaxed);
	atomic_store_explicit(&sandbox->irq.interrupted, (unsigned char)0, memory_order_relaxed);

	sandbox->slot = luaext_watchdog_acquire(&sandbox->irq);

	/*
	 * The hook goes on unconditionally, before any script can exist, and is
	 * never removed. It is the correctness mechanism -- the only thing covering
	 * lvm.c -- so installing it lazily when the first limit is set would leave a
	 * window in which a script compiled and ran with nothing watching it.
	 *
	 * LUA_MASKCOUNT only: a line hook would fire orders of magnitude more often
	 * for no extra coverage, and a call/return hook misses a loop body entirely.
	 */
	if (luaext_timers_hook_armed()) {
		lua_sethook(sandbox->L, luaext_timers_hook, LUA_MASKCOUNT, (int)LUAEXT_G(hook_count));
	}

	/*
	 * The limits the host configured take effect from construction, not from
	 * the first setCpuLimit() call. A SandboxConfig carrying cpuSeconds that
	 * nothing ever armed would be the exact failure this extension exists to
	 * eliminate -- a limit accepted and not enforced -- and it would be
	 * invisible, because the sandbox would work perfectly right up until a
	 * script decided not to stop.
	 *
	 * Both setters refuse rather than silently degrade, so a config asking for a
	 * limit this build cannot honour fails CONSTRUCTION. That is deliberate: the
	 * alternative is handing back a sandbox that quietly enforces less than it
	 * was asked to.
	 */
	if (!luaext_timers_set_cpu_limit(sandbox, sandbox->policy.limits.cpu_ns) ||
		!luaext_timers_set_wall_limit(sandbox, sandbox->policy.limits.wall_ns)) {
		return false;
	}

	return true;
}

void luaext_timers_detach(luaext_sandbox *sandbox)
{
	luaext_watch_slot *slot = sandbox->slot;

	sandbox->slot = NULL;

	luaext_watchdog_release(slot);

	/*
	 * Teardown runs with the interrupt RAISED, which is the opposite of what it
	 * looks like it should do, so here is the reasoning.
	 *
	 * lua_close() runs every pending __gc finaliser. Those are untrusted Lua,
	 * and by this point the slot has gone, so nothing is measuring anything: a
	 * script that registers
	 *
	 *     setmetatable({}, {__gc = function() while true do end end})
	 *
	 * and is then stopped by its CPU limit would hang close() forever. That is a
	 * denial of service against the whole PHP process, reachable from one line
	 * of sandboxed code, and it is worse than anything it would be trading away.
	 *
	 * What it trades away is small. A finaliser written in C -- which is every
	 * finaliser the extension itself installs, including the VFS handles -- never
	 * ticks the count hook and is not affected at all. Only a script-defined
	 * finaliser is cut short, and only at a point where the interpreter and
	 * everything it could still touch are being destroyed in the same call.
	 *
	 * The error raised inside a finaliser here is caught by GCTM's own protected
	 * call and, because lua_close() has no error handler above it, warned away
	 * rather than propagated -- so the collector finishes its list and the state
	 * closes. See the vendored patch, which checks L->errorJmp for exactly this.
	 */
	luaext_timers_request(sandbox, LUAEXT_IRQ_ABORT);
}

/* -------------------------------------------------------------------------
 * Limits
 * ---------------------------------------------------------------------- */

static bool luaext_timers_refuse(const char *what, const char *why)
{
	zend_throw_exception_ex(luaext_ce_configuration_error, 0,
							"DevelopGravity\\LuaExt\\Sandbox::%s() cannot be honoured: %s. "
							"Sandbox::features() reports the same thing before you get here.",
							what, why);

	return false;
}

bool luaext_timers_set_cpu_limit(luaext_sandbox *sandbox, uint64_t ns)
{
	if (ns != 0) {
		if (!luaext_timers_hook_armed()) {
			return luaext_timers_refuse(
				"setCpuLimit", "luaext.hook_count is 0, which removes the interpreter hook every "
							   "time limit is delivered through");
		}

		if (!luaext_timers_have_cpu) {
			return luaext_timers_refuse("setCpuLimit",
										"this platform exposes no per-thread CPU clock");
		}

		if (sandbox->slot == NULL) {
			return luaext_timers_refuse("setCpuLimit",
										"no watchdog slot could be allocated for this sandbox");
		}
	}

	if (sandbox->slot != NULL) {
		/*
		 * The degraded decision is made HERE, not in features(): that method is
		 * static and has no limit to judge. A limit close to the clock's own
		 * resolution gets a wall-clock companion deadline so the script still
		 * stops -- and when that companion trips it reports CpuLimitError,
		 * because the host asked for a CPU limit and was told the platform is
		 * coarse.
		 */
		luaext_watchdog_set_cpu_limit(sandbox->slot, ns, luaext_timers_resolution_ns);

		/*
		 * A limit set from inside a host callback has to take effect for the
		 * call that is already running, not merely the next one. Without this,
		 * a sandbox constructed with no limit could be handed one mid-run and
		 * still finish unbounded. Arming is idempotent, so this is a no-op on
		 * the ordinary path where the limit was set before the call.
		 */
		if (sandbox->in_lua > 0) {
			luaext_watchdog_arm(sandbox->slot);
		}
	}

	return true;
}

bool luaext_timers_set_wall_limit(luaext_sandbox *sandbox, uint64_t ns)
{
	if (ns != 0) {
		if (!luaext_timers_hook_armed()) {
			return luaext_timers_refuse(
				"setWallClockLimit",
				"luaext.hook_count is 0, which removes the interpreter hook every time limit is "
				"delivered through");
		}

		if (sandbox->slot == NULL) {
			return luaext_timers_refuse("setWallClockLimit",
										"no watchdog slot could be allocated for this sandbox");
		}
	}

	if (sandbox->slot != NULL) {
		luaext_watchdog_set_wall_limit(sandbox->slot, ns);

		/* Takes effect for the call already running; see setCpuLimit. */
		if (sandbox->in_lua > 0) {
			luaext_watchdog_arm(sandbox->slot);
		}
	}

	return true;
}

/* -------------------------------------------------------------------------
 * The bracket around every entry into the interpreter
 * ---------------------------------------------------------------------- */

void luaext_timers_enter_lua(luaext_sandbox *sandbox, luaext_watch_frame *frame)
{
	frame->prev_allow_pause = (uint8_t)(sandbox->allow_pause ? 1 : 0);
	frame->was_paused = luaext_watchdog_pause_mask(sandbox->slot);

	if (sandbox->in_lua == 0) {
		/*
		 * The outermost entry, and the ONLY one that arms. A nested call made
		 * from inside a host callback must not restart the clock, or a script
		 * could reset its own budget by bouncing through PHP.
		 */
		luaext_watchdog_arm(sandbox->slot);
		sandbox->allow_pause = true;
	} else {
		/*
		 * A nested entry, i.e. the host called back into Lua from inside a
		 * callback. Two things happen, and together they are the nesting rule
		 * the reference implementation pins down.
		 *
		 * First, Lua time always counts: whatever the enclosing callback paused
		 * is reopened for the duration of this call. A script must not be able
		 * to run unbilled just because the PHP frame above it paused.
		 *
		 * Second, whether the code below may pause AT ALL is decided by whether
		 * the chain above it did. If any enclosing frame chose not to pause,
		 * then it is being billed, and a deeper frame must not un-bill it. That
		 * is what makes "paused-PHP to PHP to paused-PHP" count while
		 * "paused-PHP to paused-PHP" does not.
		 */
		sandbox->allow_pause = frame->was_paused != 0;

		if (frame->was_paused != 0) {
			(void)luaext_watchdog_resume(sandbox->slot, frame->was_paused);
		}
	}

	sandbox->in_lua++;
}

void luaext_timers_leave_lua(luaext_sandbox *sandbox, const luaext_watch_frame *frame)
{
	if (sandbox->in_lua > 0) {
		sandbox->in_lua--;
	}

	if (sandbox->in_lua == 0) {
		luaext_watchdog_disarm(sandbox->slot);

		/*
		 * Where the sticky interrupt flag is finally dropped, and the reason it
		 * happens here rather than inside disarm(): Sandbox::interrupt() can
		 * raise the flag on a sandbox that has no limit and was never armed, and
		 * a flag left standing after the call it aborted would poison every
		 * subsequent one. Unconditional, because "was this armed?" is not the
		 * question -- "has this call finished?" is.
		 */
		atomic_store_explicit(&sandbox->irq.interrupted, (unsigned char)0, memory_order_relaxed);
		atomic_store_explicit(&sandbox->irq.reason, (unsigned char)LUAEXT_IRQ_NONE,
							  memory_order_relaxed);
	} else {
		uint8_t live = luaext_watchdog_pause_mask(sandbox->slot);
		uint8_t restore = (uint8_t)(frame->was_paused & ~live);
		uint8_t release = (uint8_t)(live & ~frame->was_paused);

		/*
		 * Put the pause state back exactly as this frame found it. A callback
		 * deeper in that paused and never resumed does not get to leak its pause
		 * outward, and one that resumed something its caller had paused does not
		 * get to leave its caller billed.
		 */
		if (release != 0) {
			(void)luaext_watchdog_resume(sandbox->slot, release);
		}

		if (restore != 0) {
			luaext_watchdog_pause(sandbox->slot, restore);
		}
	}

	sandbox->allow_pause = frame->prev_allow_pause != 0;
}

/* -------------------------------------------------------------------------
 * Pausing
 * ---------------------------------------------------------------------- */

bool luaext_timers_may_pause(const luaext_sandbox *sandbox)
{
	return sandbox->allow_pause;
}

bool luaext_timers_pause(luaext_sandbox *sandbox, uint8_t mask)
{
	if (!sandbox->allow_pause || mask == 0) {
		return false;
	}

	luaext_watchdog_pause(sandbox->slot, mask);

	return true;
}

void luaext_timers_resume(luaext_sandbox *sandbox, uint8_t mask)
{
	if (mask == 0) {
		return;
	}

	/*
	 * The return says the budget was ALREADY spent when the segment reopened,
	 * and it is discarded on purpose: the watchdog has by then raised the
	 * interrupt itself, with the reason it actually measured. Re-raising here
	 * would mean guessing between CPU and wall and getting it wrong half the
	 * time. Delivery is the next hook tick or LUAEXT_CHECK, which is also the
	 * only safe answer -- this is reachable from a PHP method body, where a
	 * longjmp has nothing to unwind to.
	 */
	(void)luaext_watchdog_resume(sandbox->slot, mask);
}

void luaext_timers_php_returned(luaext_sandbox *sandbox)
{
	/*
	 * A callback that paused and forgot to resume does not get to keep the
	 * pause. Everything is reopened unconditionally: the callback was entered
	 * from Lua, and Lua time always counts, so there is no earlier pause state
	 * worth preserving here. The nesting cases where there IS one are handled by
	 * leave_lua, which knows what its own frame found.
	 */
	uint8_t paused = luaext_watchdog_pause_mask(sandbox->slot);

	if (paused != 0) {
		luaext_timers_resume(sandbox, paused);
	}
}

/* -------------------------------------------------------------------------
 * Usage
 * ---------------------------------------------------------------------- */

double luaext_timers_cpu_seconds(const luaext_sandbox *sandbox)
{
	return (double)luaext_watchdog_cpu_ns(sandbox->slot) / 1e9;
}

double luaext_timers_wall_seconds(const luaext_sandbox *sandbox)
{
	return (double)luaext_watchdog_wall_ns(sandbox->slot) / 1e9;
}

/* -------------------------------------------------------------------------
 * Interrupt request
 * ---------------------------------------------------------------------- */

void luaext_timers_request(luaext_sandbox *sandbox, luaext_irq_reason reason)
{
	/*
	 * Reachable from a FOREIGN thread through Sandbox::interrupt(), so it may
	 * touch nothing but these two atomics -- not a lock, not the slot, not even
	 * `closed`. Reason first and relaxed, then the flag with release, so a
	 * reader that observes the flag also observes the reason.
	 */
	atomic_store_explicit(&sandbox->irq.reason, (unsigned char)reason, memory_order_relaxed);
	atomic_store_explicit(&sandbox->irq.interrupted, (unsigned char)1, memory_order_release);
}

/* -------------------------------------------------------------------------
 * The always-armed count hook
 * ---------------------------------------------------------------------- */

/*
 * Whether the state's current hook is ours.
 *
 * Asked by the vendored lgc.c, which disables hooks around every __gc finaliser
 * because an arbitrary Lua hook function may allocate, yield or re-enter the
 * collector. Ours is a C function that allocates nothing and either returns or
 * raises, so upstream's reason does not apply to it -- and with hooks off there
 * is nothing in the interpreter that can interrupt a finaliser's dispatch loop
 * at all, which makes an infinite __gc unstoppable.
 *
 * Declared in the vendored luaext_lua_hooks.h, which knows nothing about PHP.
 */
int luaext_hook_is_ours(lua_State *L)
{
	return lua_gethook(L) == luaext_timers_hook;
}

/*
 * The last line of defence, at the boundary where a call into Lua returns.
 *
 * Lua has places the sandbox cannot patch away where an error simply stops
 * travelling: a stock pcall catches one, and GCTM turns one into a warning. The
 * interrupt flag is sticky precisely so that a breach survives all of them -- so
 * a call that returns "successfully" with the flag still raised did not succeed,
 * and saying otherwise would report a limit that was in fact escaped.
 *
 * This is independent of the sandbox's pcall replacement rather than a
 * substitute for it: the replacement stops the script AT the pcall, which is
 * both faster and gives the right traceback. This catches whatever gets past.
 */
bool luaext_timers_throw_if_interrupted(luaext_sandbox *sandbox)
{
	zend_class_entry *ce;
	const char *what;

	if (!atomic_load_explicit(&sandbox->irq.interrupted, memory_order_relaxed)) {
		return false;
	}

	atomic_thread_fence(memory_order_acquire);

	switch ((luaext_irq_reason)atomic_load_explicit(&sandbox->irq.reason, memory_order_relaxed)) {
	case LUAEXT_IRQ_CPU:
		ce = luaext_ce_cpu_limit_error;
		what = "CPU limit";
		break;

	case LUAEXT_IRQ_WALL:
		ce = luaext_ce_wall_clock_limit_error;
		what = "wall-clock limit";
		break;

	case LUAEXT_IRQ_OUTPUT:
		ce = luaext_ce_output_limit_error;
		what = "output limit";
		break;

	default:
		ce = luaext_ce_host_abort_error;
		what = "host interrupt";
		break;
	}

	zend_throw_exception_ex(ce, 0,
							"The script returned while its %s was still raised, which means "
							"something inside it caught the breach and carried on. The result is "
							"discarded: a limit a script can survive is not a limit.",
							what);

	return true;
}

void luaext_timers_hook(lua_State *L, lua_Debug *ar)
{
	luaext_sandbox *sandbox = LUAEXT_SB(L);

	(void)ar;

	if (sandbox == NULL) {
		return;
	}

	/*
	 * The flag first, because it is a relaxed load of one byte and covers every
	 * writer: the watchdog thread, Sandbox::interrupt() from a foreign thread,
	 * and the output sink overflowing inside a frame that could not raise.
	 */
	LUAEXT_CHECK(L);

	/*
	 * Then the strided clock self-check. This is what makes the CPU limit
	 * independent of the watchdog thread entirely: CPU can only be consumed
	 * where instructions execute, so the thread executing them is in the best
	 * position to notice its own overrun. Failing to start the watchdog thread
	 * therefore degrades the WALL limit and leaves this one intact.
	 */
	if (luaext_watchdog_self_check(sandbox->slot)) {
		luaext_raise_interrupt(L);
	}
}
