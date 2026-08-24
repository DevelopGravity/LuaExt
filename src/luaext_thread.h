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

/*
 * The four types are COMPLETE here rather than opaque, because every function
 * below takes a pointer to caller-owned storage: the watchdog embeds its lock,
 * its condition variable and its once-flag directly in its own file-scope
 * state. An incomplete type would make that unwritable and would force a
 * heap allocation and a null check onto the one path that must not fail.
 *
 * Only platform threading headers are pulled in, and only the ones this
 * translation unit's consumers already depend on. Still no php.h and no lua.h:
 * that is the property the CI purity grep checks.
 */
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <pthread.h>
#endif

typedef struct luaext_mutex {
#if defined(_WIN32)
	SRWLOCK lock;
#else
	pthread_mutex_t lock;
#endif
} luaext_mutex;

typedef struct luaext_cond {
#if defined(_WIN32)
	CONDITION_VARIABLE cond;
#else
	pthread_cond_t cond;

	/*
	 * Whether the condition variable was successfully bound to a monotonic
	 * clock at init. When it was, a timed wait converts its relative deadline
	 * against CLOCK_MONOTONIC and a system clock step cannot move it; when it
	 * was not, the wait falls back to CLOCK_REALTIME and a step costs at worst
	 * one spurious or one late wakeup, which the caller's predicate re-test
	 * already tolerates. macOS takes neither path: it has a genuinely relative
	 * wait, and that is what is used there.
	 */
	bool monotonic;
#endif
} luaext_cond;

typedef struct luaext_thread {
#if defined(_WIN32)
	HANDLE handle;
#else
	pthread_t handle;
#endif
	bool started;
} luaext_thread;

typedef struct luaext_once {
#if defined(_WIN32)
	INIT_ONCE once;
#else
	pthread_once_t once;
#endif
} luaext_once;

/* Static initialisers. A once-flag in particular has to be usable before any
 * code has run, which is the whole point of it. */
#if defined(_WIN32)
#define LUAEXT_ONCE_INIT                                                                           \
	{                                                                                              \
		INIT_ONCE_STATIC_INIT                                                                      \
	}
#else
#define LUAEXT_ONCE_INIT                                                                           \
	{                                                                                              \
		PTHREAD_ONCE_INIT                                                                          \
	}
#endif

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
