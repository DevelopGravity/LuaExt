/*
 * luaext — threading primitives for the watchdog.
 *
 * SCAFFOLD. PURITY RULE: no php.h, no lua.h. See luaext_watchdog.c.
 */

#include "luaext_thread.h"

struct luaext_mutex {
	int placeholder;
};
struct luaext_cond {
	int placeholder;
};
struct luaext_thread {
	int placeholder;
};
struct luaext_once {
	int placeholder;
};

bool luaext_mutex_init(luaext_mutex *mutex)
{
	(void)mutex;
	return false;
}

void luaext_mutex_destroy(luaext_mutex *mutex)
{
	(void)mutex;
}

void luaext_mutex_lock(luaext_mutex *mutex)
{
	(void)mutex;
}

void luaext_mutex_unlock(luaext_mutex *mutex)
{
	(void)mutex;
}

bool luaext_cond_init(luaext_cond *cond)
{
	(void)cond;
	return false;
}

void luaext_cond_destroy(luaext_cond *cond)
{
	(void)cond;
}

void luaext_cond_signal(luaext_cond *cond)
{
	(void)cond;
}

void luaext_cond_wait(luaext_cond *cond, luaext_mutex *mutex)
{
	(void)cond;
	(void)mutex;
}

void luaext_cond_wait_for(luaext_cond *cond, luaext_mutex *mutex, uint64_t ns)
{
	(void)cond;
	(void)mutex;
	(void)ns;
}

void luaext_once_run(luaext_once *once, void (*routine)(void))
{
	(void)once;
	(void)routine;
}

bool luaext_thread_start(luaext_thread *thread, void *(*entry)(void *), void *arg)
{
	(void)thread;
	(void)entry;
	(void)arg;
	return false;
}

void luaext_thread_join(luaext_thread *thread)
{
	(void)thread;
}

uintptr_t luaext_thread_self(void)
{
	return 0;
}
