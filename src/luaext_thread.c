/*
 * luaext — threading primitives for the watchdog.
 *
 * PURITY RULE: no php.h, no lua.h. See luaext_watchdog.c.
 *
 * Two platform families, and one deliberate asymmetry between them. Timed waits
 * are RELATIVE in this API because that is the only shape all three backends can
 * express without inheriting the bug where somebody steps the system clock and a
 * deadline lands hours away:
 *
 *   Windows  SleepConditionVariableSRW already takes a relative timeout.
 *   macOS    pthread_cond_timedwait_relative_np already takes a relative one.
 *   Linux    has neither, so the condition variable is bound to CLOCK_MONOTONIC
 *            at init and the relative timeout is converted against that clock.
 *            Where that binding is unavailable the wait degrades to
 *            CLOCK_REALTIME, which a clock step can perturb by exactly one
 *            wakeup -- harmless, because every caller re-tests its predicate.
 */

#include "luaext_thread.h"

#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32)
#include <time.h>
#endif

/*
 * Whether pthread_condattr_setclock(CLOCK_MONOTONIC) is worth attempting.
 *
 * Not on Apple: the symbol does not exist there, and the relative wait it would
 * be emulating is provided natively instead.
 */
#if !defined(_WIN32) && !defined(__APPLE__) && defined(CLOCK_MONOTONIC) &&                          \
	!defined(LUAEXT_NO_COND_MONOTONIC)
#define LUAEXT_COND_TRY_MONOTONIC 1
#else
#define LUAEXT_COND_TRY_MONOTONIC 0
#endif

#define LUAEXT_NS_PER_SEC UINT64_C(1000000000)

/* -------------------------------------------------------------------------
 * Mutexes
 * ---------------------------------------------------------------------- */

bool luaext_mutex_init(luaext_mutex *mutex)
{
#if defined(_WIN32)
	InitializeSRWLock(&mutex->lock);
	return true;
#else
	return pthread_mutex_init(&mutex->lock, NULL) == 0;
#endif
}

void luaext_mutex_destroy(luaext_mutex *mutex)
{
#if defined(_WIN32)
	/* An SRWLOCK owns nothing and has no destructor. */
	(void)mutex;
#else
	(void)pthread_mutex_destroy(&mutex->lock);
#endif
}

void luaext_mutex_lock(luaext_mutex *mutex)
{
#if defined(_WIN32)
	AcquireSRWLockExclusive(&mutex->lock);
#else
	(void)pthread_mutex_lock(&mutex->lock);
#endif
}

void luaext_mutex_unlock(luaext_mutex *mutex)
{
#if defined(_WIN32)
	ReleaseSRWLockExclusive(&mutex->lock);
#else
	(void)pthread_mutex_unlock(&mutex->lock);
#endif
}

/* -------------------------------------------------------------------------
 * Condition variables
 * ---------------------------------------------------------------------- */

bool luaext_cond_init(luaext_cond *cond)
{
#if defined(_WIN32)
	InitializeConditionVariable(&cond->cond);
	return true;
#else
#if LUAEXT_COND_TRY_MONOTONIC
	{
		pthread_condattr_t attributes;

		if (pthread_condattr_init(&attributes) == 0) {
			bool bound = pthread_condattr_setclock(&attributes, CLOCK_MONOTONIC) == 0;
			bool created = pthread_cond_init(&cond->cond, bound ? &attributes : NULL) == 0;

			(void)pthread_condattr_destroy(&attributes);

			if (created) {
				cond->monotonic = bound;
				return true;
			}
		}
	}
#endif

	cond->monotonic = false;

	return pthread_cond_init(&cond->cond, NULL) == 0;
#endif
}

void luaext_cond_destroy(luaext_cond *cond)
{
#if defined(_WIN32)
	(void)cond;
#else
	(void)pthread_cond_destroy(&cond->cond);
#endif
}

void luaext_cond_signal(luaext_cond *cond)
{
#if defined(_WIN32)
	WakeConditionVariable(&cond->cond);
#else
	(void)pthread_cond_signal(&cond->cond);
#endif
}

void luaext_cond_wait(luaext_cond *cond, luaext_mutex *mutex)
{
#if defined(_WIN32)
	(void)SleepConditionVariableSRW(&cond->cond, &mutex->lock, INFINITE, 0);
#else
	(void)pthread_cond_wait(&cond->cond, &mutex->lock);
#endif
}

void luaext_cond_wait_for(luaext_cond *cond, luaext_mutex *mutex, uint64_t ns)
{
#if defined(_WIN32)
	/*
	 * Rounded UP to the next millisecond. Rounding down would let a sub-tick
	 * deadline become a zero-length wait, and a zero-length wait in a loop is a
	 * spin: the watchdog would burn a core waiting for a deadline it cannot
	 * express. One millisecond late is the honest answer on a platform whose
	 * scheduler tick is fifteen.
	 */
	uint64_t ms = (ns + UINT64_C(999999)) / UINT64_C(1000000);

	if (ms > INFINITE - 1) {
		ms = INFINITE - 1;
	}

	(void)SleepConditionVariableSRW(&cond->cond, &mutex->lock, (DWORD)ms, 0);
#elif defined(__APPLE__)
	struct timespec relative;

	relative.tv_sec = (time_t)(ns / LUAEXT_NS_PER_SEC);
	relative.tv_nsec = (long)(ns % LUAEXT_NS_PER_SEC);

	(void)pthread_cond_timedwait_relative_np(&cond->cond, &mutex->lock, &relative);
#else
	struct timespec deadline;
	uint64_t total;

	if (clock_gettime(cond->monotonic ? CLOCK_MONOTONIC : CLOCK_REALTIME, &deadline) != 0) {
		/* Degrading to an untimed wait would hang the watchdog until the next
		 * signal, so a clock that cannot be read costs a poll instead. */
		(void)pthread_cond_wait(&cond->cond, &mutex->lock);
		return;
	}

	total = (uint64_t)deadline.tv_nsec + ns;
	deadline.tv_sec += (time_t)(total / LUAEXT_NS_PER_SEC);
	deadline.tv_nsec = (long)(total % LUAEXT_NS_PER_SEC);

	(void)pthread_cond_timedwait(&cond->cond, &mutex->lock, &deadline);
#endif
}

/* -------------------------------------------------------------------------
 * Run-once
 * ---------------------------------------------------------------------- */

#if defined(_WIN32)
static BOOL CALLBACK luaext_once_adapter(PINIT_ONCE once, PVOID parameter, PVOID *context)
{
	/*
	 * InitOnceExecuteOnce only carries a void *, so the routine travels as one.
	 * ISO C does not guarantee that round trip; every Windows ABI does, and the
	 * alternative is a file-scope slot that would itself need the ordering this
	 * call is being used to establish.
	 */
	void (*routine)(void) = (void (*)(void))parameter;

	(void)once;
	(void)context;

	routine();

	return TRUE;
}
#endif

void luaext_once_run(luaext_once *once, void (*routine)(void))
{
#if defined(_WIN32)
	(void)InitOnceExecuteOnce(&once->once, luaext_once_adapter, (PVOID)routine, NULL);
#else
	(void)pthread_once(&once->once, routine);
#endif
}

/* -------------------------------------------------------------------------
 * Threads
 * ---------------------------------------------------------------------- */

#if defined(_WIN32)
typedef struct {
	void *(*entry)(void *);
	void *argument;
} luaext_thread_trampoline;

static DWORD WINAPI luaext_thread_entry(LPVOID parameter)
{
	luaext_thread_trampoline *trampoline = (luaext_thread_trampoline *)parameter;
	void *(*entry)(void *) = trampoline->entry;
	void *argument = trampoline->argument;

	/* Freed here rather than by the starter: the starter returns as soon as the
	 * thread exists and would otherwise be racing this read. */
	free(trampoline);

	(void)entry(argument);

	return 0;
}
#endif

bool luaext_thread_start(luaext_thread *thread, void *(*entry)(void *), void *arg)
{
#if defined(_WIN32)
	luaext_thread_trampoline *trampoline;

	thread->started = false;
	thread->handle = NULL;

	trampoline = (luaext_thread_trampoline *)malloc(sizeof(*trampoline));

	if (trampoline == NULL) {
		return false;
	}

	trampoline->entry = entry;
	trampoline->argument = arg;

	thread->handle = CreateThread(NULL, 0, luaext_thread_entry, trampoline, 0, NULL);

	if (thread->handle == NULL) {
		free(trampoline);
		return false;
	}

	thread->started = true;

	return true;
#else
	thread->started = pthread_create(&thread->handle, NULL, entry, arg) == 0;

	return thread->started;
#endif
}

void luaext_thread_join(luaext_thread *thread)
{
	if (!thread->started) {
		return;
	}

	thread->started = false;

#if defined(_WIN32)
	(void)WaitForSingleObject(thread->handle, INFINITE);
	(void)CloseHandle(thread->handle);
	thread->handle = NULL;
#else
	(void)pthread_join(thread->handle, NULL);
#endif
}

uintptr_t luaext_thread_self(void)
{
#if defined(_WIN32)
	return (uintptr_t)GetCurrentThreadId();
#else
	/*
	 * pthread_t is a pointer on some platforms and an integer on others, and
	 * nothing says it is castable. Copying its bytes gives a value that compares
	 * equal exactly when the two pthread_t values are bitwise identical, which
	 * is all this is ever used for.
	 */
	pthread_t self = pthread_self();
	uintptr_t identity = 0;
	size_t copied = sizeof(self) < sizeof(identity) ? sizeof(self) : sizeof(identity);

	memcpy(&identity, &self, copied);

	return identity;
#endif
}
