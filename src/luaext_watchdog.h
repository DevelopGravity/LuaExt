/*
 * luaext — the deadline watchdog.
 *
 * THIS HEADER AND ITS IMPLEMENTATION ARE PHP-FREE AND LUA-FREE, and that is a
 * security property rather than a matter of taste.
 *
 * The watchdog runs on a thread that PHP did not create. Under a worker SAPI
 * like FrankenPHP there are many PHP threads, each with its own module globals,
 * and the watchdog belongs to none of them: it has no TSRM context. A
 * LUAEXT_G(...) from here would not fail to compile and would not crash -- it
 * would quietly read some other thread's globals. That is worse than a crash.
 *
 * So the slot holds a `luaext_irq *`, never a `luaext_sandbox *`. The watchdog
 * is then physically incapable of reaching a zend_object, the live-sandbox
 * list, a class entry or the interpreter, because none of those types are in
 * scope. The CI grep in lint.yml is a second, independent check on the same
 * property.
 *
 * How it sleeps: thread CPU time advances no faster than wall time, so a
 * sandbox's remaining CPU budget is a valid lower bound on the earliest moment
 * it could possibly trip. The watchdog sleeps exactly that long rather than
 * polling. A CPU-bound one-second limit therefore costs about one wakeup.
 */

#ifndef LUAEXT_WATCHDOG_H
#define LUAEXT_WATCHDOG_H

#include "luaext_clock.h"
#include "luaext_thread.h"

#include "luaext_lua_hooks.h" /* luaext_irq only -- no lua.h, no php.h */

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct luaext_watch_slot luaext_watch_slot;

/* Which budgets a pause covers. Mirrors LUAEXT_TIMER_* in luaext_timers.h;
 * duplicated rather than shared because that header includes php.h. */
#define LUAEXT_WATCH_CPU (1u << 0)
#define LUAEXT_WATCH_WALL (1u << 1)

/* -------------------------------------------------------------------------
 * Module lifecycle
 * ---------------------------------------------------------------------- */

void luaext_watchdog_startup(void);

/*
 * Stop the thread, JOIN it, and only then release the pool.
 *
 * The ordering is the point: after the join, no thread can be reading a slot,
 * so releasing the backing store is safe. Reversing it turns a clean shutdown
 * into a use-after-free that only shows up under load.
 */
void luaext_watchdog_shutdown(void);

/* -------------------------------------------------------------------------
 * Slots
 *
 * A slot is taken at sandbox construction and returned at close. The backing
 * store is never freed before shutdown: the deadline heap holds raw slot
 * pointers, so a freed slot would leave a window no amount of generation
 * checking could close -- you cannot read a generation counter out of memory
 * that has been handed back. Keeping the store alive turns a lifetime problem
 * into a validity problem, which a generation counter does solve.
 * ---------------------------------------------------------------------- */

/* Capture the CALLING thread's CPU clock into the slot; must run on the thread
 * that will own the sandbox. */
luaext_watch_slot *luaext_watchdog_acquire(luaext_irq *irq);

/* Detach and return the slot. Idempotent. Never takes the watchdog lock: this
 * runs on every sandbox teardown, and making the request path contend for a
 * process-wide lock would be a real bottleneck under a worker SAPI. Any
 * outstanding heap entry is invalidated by the generation bump and reaped
 * lazily when it surfaces. */
void luaext_watchdog_release(luaext_watch_slot *slot);

/* -------------------------------------------------------------------------
 * Limits and accounting. Nanoseconds; 0 means no limit.
 * ---------------------------------------------------------------------- */

void luaext_watchdog_set_cpu_limit(luaext_watch_slot *slot, uint64_t ns, uint64_t resolution_ns);
void luaext_watchdog_set_wall_limit(luaext_watch_slot *slot, uint64_t ns);

/* Open both segments and publish a deadline. Only the outermost entry into the
 * interpreter calls this. */
void luaext_watchdog_arm(luaext_watch_slot *slot);
void luaext_watchdog_disarm(luaext_watch_slot *slot);

/*
 * Close or reopen the segments named by `mask`.
 *
 * Nothing can expire while paused, because while paused nothing is measured --
 * which is why this design needs none of the "the limit went off while we were
 * not looking" reconstruction the extension it replaces carries.
 *
 * luaext_watchdog_resume() returns true when the budget is ALREADY spent, and
 * the caller must trip immediately rather than waiting for a wakeup that would
 * grant the script a whole extra budget.
 */
void luaext_watchdog_pause(luaext_watch_slot *slot, uint8_t mask);
bool luaext_watchdog_resume(luaext_watch_slot *slot, uint8_t mask);

uint8_t luaext_watchdog_pause_mask(const luaext_watch_slot *slot);

uint64_t luaext_watchdog_cpu_ns(const luaext_watch_slot *slot);
uint64_t luaext_watchdog_wall_ns(const luaext_watch_slot *slot);

/*
 * Evaluate this slot from the owning thread and trip it if it is over budget.
 *
 * Called on a stride from the count hook. This is what makes the CPU limit
 * independent of the watchdog thread: CPU is only consumed where instructions
 * execute, so the thread executing them can notice its own overrun.
 *
 * Returns true when the caller must raise. It must do so AFTER this returns --
 * raising is a longjmp, and the slot lock is held inside.
 */
bool luaext_watchdog_self_check(luaext_watch_slot *slot);

/* True when the watchdog thread is running. False means the wall limit
 * degrades to "trips when Lua next executes an instruction", which
 * Sandbox::features() must report rather than conceal. */
bool luaext_watchdog_thread_running(void);

#endif /* LUAEXT_WATCHDOG_H */
