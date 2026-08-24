/*
 * luaext — the threading primitives the watchdog is built from.
 *
 * PHP-free and Lua-free by design; see luaext_clock.h for why.
 *
 * Waits are always RELATIVE. pthread_condattr_setclock(CLOCK_MONOTONIC) exists
 * on Linux but not on macOS, so an absolute-deadline API would need a different
 * implementation per platform and would inherit the whole class of bugs where
 * somebody sets the system clock and a deadline lands in the far future.
 */

#ifndef LUAEXT_THREAD_H
#define LUAEXT_THREAD_H

#include <stdbool.h>
#include <stdint.h>

typedef struct luaext_mutex luaext_mutex;
typedef struct luaext_cond luaext_cond;
typedef struct luaext_thread luaext_thread;
typedef struct luaext_once luaext_once;

bool luaext_mutex_init(luaext_mutex *mutex);
void luaext_mutex_destroy(luaext_mutex *mutex);
void luaext_mutex_lock(luaext_mutex *mutex);
void luaext_mutex_unlock(luaext_mutex *mutex);

bool luaext_cond_init(luaext_cond *cond);
void luaext_cond_destroy(luaext_cond *cond);
void luaext_cond_signal(luaext_cond *cond);

/* Wait until signalled. The mutex is released while waiting and reacquired
 * before returning, as usual. */
void luaext_cond_wait(luaext_cond *cond, luaext_mutex *mutex);

/* Wait until signalled or `ns` nanoseconds have passed. Spurious wakeups are
 * possible, so the caller re-tests its predicate regardless. */
void luaext_cond_wait_for(luaext_cond *cond, luaext_mutex *mutex, uint64_t ns);

/*
 * Run `routine` exactly once across every thread that reaches this point.
 *
 * The watchdog is created lazily on the first armed limit, and under a worker
 * SAPI several PHP threads can reach that point together. This also supplies
 * the happens-before edge that lets every caller read whatever the routine
 * wrote without further synchronisation.
 */
void luaext_once_run(luaext_once *once, void (*routine)(void));

bool luaext_thread_start(luaext_thread *thread, void *(*entry)(void *), void *arg);
void luaext_thread_join(luaext_thread *thread);

/* Identifies the thread that owns a sandbox. Only ever compared, never used to
 * reach a thread -- values are recycled after a thread exits. */
uintptr_t luaext_thread_self(void);

#endif /* LUAEXT_THREAD_H */
