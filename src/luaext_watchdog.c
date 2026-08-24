/*
 * luaext — the deadline watchdog.
 *
 * SCAFFOLD. No thread, no pool, no heap yet.
 *
 * PURITY RULE, enforced structurally and by a CI grep: this file includes
 * neither php.h nor lua.h, and must not gain either. The watchdog runs on a
 * thread PHP did not create and that has no TSRM context, so a LUAEXT_G() from
 * here would silently read another thread's globals rather than failing.
 * Keeping those headers out of scope makes the mistake unwritable.
 */

#include "luaext_watchdog.h"

struct luaext_watch_slot {
	int placeholder;
};

void luaext_watchdog_startup(void) {}

void luaext_watchdog_shutdown(void) {}

luaext_watch_slot *luaext_watchdog_acquire(luaext_irq *irq)
{
	(void)irq;
	return NULL;
}

void luaext_watchdog_release(luaext_watch_slot *slot)
{
	(void)slot;
}

void luaext_watchdog_set_cpu_limit(luaext_watch_slot *slot, uint64_t ns, uint64_t resolution_ns)
{
	(void)slot;
	(void)ns;
	(void)resolution_ns;
}

void luaext_watchdog_set_wall_limit(luaext_watch_slot *slot, uint64_t ns)
{
	(void)slot;
	(void)ns;
}

void luaext_watchdog_arm(luaext_watch_slot *slot)
{
	(void)slot;
}

void luaext_watchdog_disarm(luaext_watch_slot *slot)
{
	(void)slot;
}

void luaext_watchdog_pause(luaext_watch_slot *slot, uint8_t mask)
{
	(void)slot;
	(void)mask;
}

bool luaext_watchdog_resume(luaext_watch_slot *slot, uint8_t mask)
{
	(void)slot;
	(void)mask;
	return false;
}

uint8_t luaext_watchdog_pause_mask(const luaext_watch_slot *slot)
{
	(void)slot;
	return 0;
}

uint64_t luaext_watchdog_cpu_ns(const luaext_watch_slot *slot)
{
	(void)slot;
	return 0;
}

uint64_t luaext_watchdog_wall_ns(const luaext_watch_slot *slot)
{
	(void)slot;
	return 0;
}

bool luaext_watchdog_self_check(luaext_watch_slot *slot)
{
	(void)slot;
	return false;
}

bool luaext_watchdog_thread_running(void)
{
	return false;
}
