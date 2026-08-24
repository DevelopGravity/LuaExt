/*
 * luaext — reading a specific thread's CPU clock, from any thread.
 *
 * This header is deliberately PHP-free and Lua-free. It is reached from the
 * watchdog thread, which is not a PHP thread: it has no TSRM context, so a
 * LUAEXT_G() from there would not fail loudly, it would quietly read another
 * thread's globals. Keeping php.h out of scope is what makes that unwritable
 * rather than merely discouraged.
 *
 * The one thing to get right here: a sandbox is pinned to the thread that
 * created it, but the watchdog reading its CPU time is a DIFFERENT thread. So
 * the handle has to be captured on the owning thread and stay valid for another
 * thread to read. Every platform spells that differently, and two of the three
 * have a trap that compiles perfectly and measures the wrong thing.
 */

#ifndef LUAEXT_CLOCK_H
#define LUAEXT_CLOCK_H

#include <stdbool.h>
#include <stdint.h>

/*
 * An owning thread's CPU clock, captured so another thread can read it.
 *
 *   Linux/BSD  a clockid_t from pthread_getcpuclockid(). No ownership; valid
 *              until the thread exits, then reads fail.
 *   macOS      the mach port from pthread_mach_thread_np(). Deliberately NOT
 *              mach_thread_self(), which returns a send right that must be
 *              released with mach_port_deallocate() -- forgetting that leaks a
 *              port right per sandbox, and it is an easy change to make by
 *              accident because the two look interchangeable.
 *   Windows    a real handle from DuplicateHandle(). GetCurrentThread() returns
 *              a PSEUDO-handle meaning "whichever thread is asking", so using
 *              it directly would silently measure the watchdog's own CPU.
 */
typedef struct luaext_cpu_clock {
	/*
	 * The captured handle, in whichever of the two shapes C can give a platform
	 * handle:
	 *
	 *   Windows  a real HANDLE from DuplicateHandle()          -> pointer
	 *   macOS    a mach_port_t from pthread_mach_thread_np()   -> identifier
	 *   POSIX    a clockid_t from pthread_getcpuclockid()      -> identifier
	 *
	 * Spelled this way, rather than as the platform types themselves, because
	 * the watchdog embeds this struct by value and this header must not drag
	 * <windows.h> or <mach/mach.h> into its include graph.
	 *
	 * `identifier` is SIGNED on purpose: Linux hands out negative clockid_t
	 * values for clocks that are not the caller's own, which is exactly the case
	 * this whole file exists to serve.
	 */
	union {
		void *pointer;
		int64_t identifier;
	} handle;

	bool valid;
} luaext_cpu_clock;

/* Capture the CALLING thread's clock. Must run on the owning thread. */
bool luaext_clock_capture_self(luaext_cpu_clock *out);

/* Release whatever the capture took. Safe on any thread, and safe to repeat. */
void luaext_clock_release(luaext_cpu_clock *clock);

/*
 * Read consumed CPU nanoseconds. Callable from any thread.
 *
 * Returns false when the clock can no longer be read -- which in practice means
 * the owning thread has exited. The caller treats that as a trip rather than as
 * zero: a sandbox whose thread is gone must not look like one using no CPU.
 */
bool luaext_clock_read(const luaext_cpu_clock *clock, uint64_t *ns);

/* A monotonic timebase for wall deadlines. Never wall-clock time: it must not
 * move when somebody sets the system clock. */
uint64_t luaext_clock_monotonic_ns(void);

/*
 * The granularity of the CPU clock, in nanoseconds.
 *
 * This is the number Sandbox::features() reports, and it is the honest half of
 * the extension's central promise. Linux resolves to nanoseconds, macOS to
 * microseconds, and Windows to the ~15.6 ms scheduler tick -- which is why
 * Windows reports Degraded and arms a wall-clock companion rather than
 * pretending a 10 ms CPU limit means anything there.
 */
uint64_t luaext_clock_cpu_resolution_ns(void);

/* True when this platform can measure per-thread CPU time at all. */
bool luaext_clock_cpu_available(void);

#endif /* LUAEXT_CLOCK_H */
