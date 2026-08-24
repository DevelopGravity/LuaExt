/*
 * luaext — per-thread CPU clocks.
 *
 * PURITY RULE: no php.h, no lua.h. See luaext_watchdog.c.
 *
 * Three backends. Two of them have a trap that compiles perfectly and measures
 * the wrong thing, so each is spelled out at its capture site:
 *
 *   Linux/BSD  pthread_getcpuclockid() then clock_gettime(). Nanosecond
 *              resolution and no ownership; the clockid stops resolving once
 *              the owning thread exits.
 *   macOS      pthread_mach_thread_np(), NOT mach_thread_self(). Microsecond
 *              resolution via thread_info(THREAD_BASIC_INFO).
 *   Windows    DuplicateHandle() of the GetCurrentThread() pseudo-handle, read
 *              with GetThreadTimes(). The units are 100 ns but the underlying
 *              accounting advances on the ~15.6 ms scheduler tick, which is
 *              what makes a CPU limit Degraded there rather than Enforced.
 */

#include "luaext_clock.h"

#include <string.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <time.h>
#else
#include <pthread.h>
#include <time.h>
#endif

/* Which backend this build actually compiled. Everything below keys off these
 * rather than re-deriving the platform test, so the struct, the capture and the
 * read can never disagree about which member is live. */
#if defined(_WIN32)
#define LUAEXT_CLOCK_WINDOWS 1
#elif defined(__APPLE__)
#define LUAEXT_CLOCK_MACH 1
#elif defined(CLOCK_THREAD_CPUTIME_ID) && defined(_POSIX_THREAD_CPUTIME)
#define LUAEXT_CLOCK_POSIX 1
#elif defined(CLOCK_THREAD_CPUTIME_ID)
#define LUAEXT_CLOCK_POSIX 1
#else
#define LUAEXT_CLOCK_NONE 1
#endif

#define LUAEXT_NS_PER_SEC UINT64_C(1000000000)

/* -------------------------------------------------------------------------
 * Capture and release
 * ---------------------------------------------------------------------- */

bool luaext_clock_capture_self(luaext_cpu_clock *out)
{
	memset(out, 0, sizeof(*out));
	out->valid = false;

#if defined(LUAEXT_CLOCK_WINDOWS)
	/*
	 * GetCurrentThread() is a PSEUDO-handle: a constant meaning "whichever
	 * thread is asking". Storing it and reading it from the watchdog would
	 * silently report the WATCHDOG's CPU time, which is near zero -- so every
	 * sandbox would look as though it were using nothing. DuplicateHandle turns
	 * it into a real handle onto this specific thread.
	 */
	out->handle.pointer = NULL;

	if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(),
						 (LPHANDLE)&out->handle.pointer, THREAD_QUERY_INFORMATION, FALSE, 0)) {
		out->handle.pointer = NULL;
		return false;
	}

	out->valid = true;

	return true;
#elif defined(LUAEXT_CLOCK_MACH)
	/*
	 * pthread_mach_thread_np() returns the thread's existing port name and takes
	 * no reference, so there is nothing to release. mach_thread_self() looks
	 * interchangeable and is not: it returns a send RIGHT that must be handed
	 * back with mach_port_deallocate(), and forgetting that leaks one port right
	 * per sandbox until the process runs out of them.
	 */
	out->handle.identifier = (int64_t)pthread_mach_thread_np(pthread_self());

	if ((mach_port_t)out->handle.identifier == MACH_PORT_NULL) {
		return false;
	}

	out->valid = true;

	return true;
#elif defined(LUAEXT_CLOCK_POSIX)
	{
		clockid_t captured;

		if (pthread_getcpuclockid(pthread_self(), &captured) != 0) {
			return false;
		}

		out->handle.identifier = (int64_t)captured;
	}

	out->valid = true;

	return true;
#else
	return false;
#endif
}

void luaext_clock_release(luaext_cpu_clock *clock)
{
	if (!clock->valid) {
		return;
	}

	clock->valid = false;

#if defined(LUAEXT_CLOCK_WINDOWS)
	if (clock->handle.pointer != NULL) {
		(void)CloseHandle((HANDLE)clock->handle.pointer);
		clock->handle.pointer = NULL;
	}
#elif defined(LUAEXT_CLOCK_MACH)
	/* Nothing was taken, so nothing is given back. See the capture. */
	clock->handle.identifier = (int64_t)MACH_PORT_NULL;
#elif defined(LUAEXT_CLOCK_POSIX)
	/* A clockid_t is a value, not a resource. */
	(void)clock;
#endif
}

/* -------------------------------------------------------------------------
 * Reading
 * ---------------------------------------------------------------------- */

bool luaext_clock_read(const luaext_cpu_clock *clock, uint64_t *ns)
{
	if (!clock->valid) {
		return false;
	}

#if defined(LUAEXT_CLOCK_WINDOWS)
	{
		FILETIME created;
		FILETIME exited;
		FILETIME kernel;
		FILETIME user;
		ULARGE_INTEGER kernel_ticks;
		ULARGE_INTEGER user_ticks;

		if (!GetThreadTimes((HANDLE)clock->handle.pointer, &created, &exited, &kernel, &user)) {
			return false;
		}

		kernel_ticks.LowPart = kernel.dwLowDateTime;
		kernel_ticks.HighPart = kernel.dwHighDateTime;
		user_ticks.LowPart = user.dwLowDateTime;
		user_ticks.HighPart = user.dwHighDateTime;

		/* FILETIME counts 100 ns intervals. */
		*ns = (uint64_t)(kernel_ticks.QuadPart + user_ticks.QuadPart) * UINT64_C(100);

		return true;
	}
#elif defined(LUAEXT_CLOCK_MACH)
	{
		mach_msg_type_number_t count = THREAD_BASIC_INFO_COUNT;
		struct thread_basic_info info;

		/*
		 * Fails with KERN_INVALID_ARGUMENT once the owning thread is gone, which
		 * is exactly the signal the caller wants: a sandbox whose thread has
		 * exited must not read as one using no CPU.
		 */
		if (thread_info((mach_port_t)clock->handle.identifier, THREAD_BASIC_INFO,
						(thread_info_t)&info, &count) != KERN_SUCCESS) {
			return false;
		}

		*ns = ((uint64_t)info.user_time.seconds + (uint64_t)info.system_time.seconds) *
				  LUAEXT_NS_PER_SEC +
			  ((uint64_t)info.user_time.microseconds + (uint64_t)info.system_time.microseconds) *
				  UINT64_C(1000);

		return true;
	}
#elif defined(LUAEXT_CLOCK_POSIX)
	{
		struct timespec now;

		if (clock_gettime((clockid_t)clock->handle.identifier, &now) != 0) {
			return false;
		}

		*ns = (uint64_t)now.tv_sec * LUAEXT_NS_PER_SEC + (uint64_t)now.tv_nsec;

		return true;
	}
#else
	(void)ns;

	return false;
#endif
}

uint64_t luaext_clock_monotonic_ns(void)
{
#if defined(LUAEXT_CLOCK_WINDOWS)
	static LARGE_INTEGER frequency;
	LARGE_INTEGER counter;

	if (frequency.QuadPart == 0 && !QueryPerformanceFrequency(&frequency)) {
		return (uint64_t)GetTickCount64() * UINT64_C(1000000);
	}

	if (!QueryPerformanceCounter(&counter)) {
		return (uint64_t)GetTickCount64() * UINT64_C(1000000);
	}

	/*
	 * Split into whole seconds and a remainder before scaling. Multiplying the
	 * raw counter by a billion overflows 64 bits after about six minutes of
	 * uptime on a 10 MHz timebase, which would make every deadline nonsense on
	 * a machine that had merely been switched on for a while.
	 */
	return (uint64_t)(counter.QuadPart / frequency.QuadPart) * LUAEXT_NS_PER_SEC +
		   (uint64_t)(counter.QuadPart % frequency.QuadPart) * LUAEXT_NS_PER_SEC /
			   (uint64_t)frequency.QuadPart;
#else
	struct timespec now;

#if defined(CLOCK_MONOTONIC)
	if (clock_gettime(CLOCK_MONOTONIC, &now) == 0) {
		return (uint64_t)now.tv_sec * LUAEXT_NS_PER_SEC + (uint64_t)now.tv_nsec;
	}
#endif

	/* Never wall-clock time by preference: a deadline must not move when
	 * somebody sets the system clock. This is the last resort. */
	if (clock_gettime(CLOCK_REALTIME, &now) == 0) {
		return (uint64_t)now.tv_sec * LUAEXT_NS_PER_SEC + (uint64_t)now.tv_nsec;
	}

	return 0;
#endif
}

/* -------------------------------------------------------------------------
 * What the platform can actually resolve
 * ---------------------------------------------------------------------- */

uint64_t luaext_clock_cpu_resolution_ns(void)
{
#if defined(LUAEXT_CLOCK_WINDOWS)
	DWORD adjustment = 0;
	DWORD interval = 0;
	BOOL disabled = FALSE;

	/*
	 * The system timer interval, in 100 ns units -- ~15.6 ms on a default
	 * install. GetThreadTimes reports 100 ns units, but thread CPU accounting
	 * only advances when the tick fires, so reporting 100 would be a lie that
	 * made features() claim a precision the platform does not have.
	 */
	if (GetSystemTimeAdjustment(&adjustment, &interval, &disabled) && interval > 0) {
		return (uint64_t)interval * UINT64_C(100);
	}

	return UINT64_C(15600000);
#elif defined(LUAEXT_CLOCK_MACH)
	/* thread_info reports whole microseconds. */
	return UINT64_C(1000);
#elif defined(LUAEXT_CLOCK_POSIX)
	{
		struct timespec resolution;

		if (clock_getres(CLOCK_THREAD_CPUTIME_ID, &resolution) == 0) {
			uint64_t ns =
				(uint64_t)resolution.tv_sec * LUAEXT_NS_PER_SEC + (uint64_t)resolution.tv_nsec;

			/* A clock claiming perfect resolution is claiming something no clock
			 * has; one nanosecond is the finest honest answer. */
			return ns == 0 ? UINT64_C(1) : ns;
		}

		return UINT64_C(1);
	}
#else
	return 0;
#endif
}

bool luaext_clock_cpu_available(void)
{
#if defined(LUAEXT_CLOCK_NONE)
	return false;
#else
	/*
	 * Probed rather than assumed: a platform can ship the symbols and still
	 * refuse at runtime (a container with a restricted seccomp profile, a Mach
	 * task without thread_info rights). Capturing and reading this thread's own
	 * clock once at startup is the only answer that is not a guess.
	 */
	luaext_cpu_clock probe;
	uint64_t ns = 0;
	bool ok;

	if (!luaext_clock_capture_self(&probe)) {
		return false;
	}

	ok = luaext_clock_read(&probe, &ns);
	luaext_clock_release(&probe);

	return ok;
#endif
}
