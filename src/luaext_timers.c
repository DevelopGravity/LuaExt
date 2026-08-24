/*
 * luaext — the PHP-facing half of the CPU and wall-clock limits.
 *
 * SCAFFOLD. Nothing is enforced yet, and every entry point here says so rather
 * than pretending. In particular the limit setters REFUSE: accepting a CPU
 * limit and not enforcing it is precisely the failure this extension exists to
 * eliminate, and it is what the extension it replaces did on macOS and Windows.
 */

#include "luaext_timers.h"

#include "luaext_error.h"
#include "luaext_watchdog.h"

#include <Zend/zend_exceptions.h>

void luaext_timers_startup(void) {}

void luaext_timers_shutdown(void) {}

/* -------------------------------------------------------------------------
 * What features() reports. Honest by construction: until the watchdog and the
 * clock backends exist, neither limit is enforceable.
 * ---------------------------------------------------------------------- */

luaext_limit_support luaext_timers_cpu_support(void)
{
	return LUAEXT_LIMIT_UNSUPPORTED;
}

luaext_limit_support luaext_timers_wall_support(void)
{
	return LUAEXT_LIMIT_UNSUPPORTED;
}

double luaext_timers_cpu_resolution_seconds(void)
{
	return 0.0;
}

/* -------------------------------------------------------------------------
 * Per-sandbox lifecycle
 * ---------------------------------------------------------------------- */

bool luaext_timers_attach(luaext_sandbox *sandbox)
{
	(void)sandbox;
	return true;
}

void luaext_timers_detach(luaext_sandbox *sandbox)
{
	(void)sandbox;
}

/* -------------------------------------------------------------------------
 * Limits
 * ---------------------------------------------------------------------- */

static bool luaext_timers_unavailable(const char *what)
{
	zend_throw_error(NULL,
					 "DevelopGravity\\LuaExt\\Sandbox::%s() is not implemented yet: this build "
					 "cannot enforce a time limit, and Sandbox::features() reports so",
					 what);
	return false;
}

bool luaext_timers_set_cpu_limit(luaext_sandbox *sandbox, uint64_t ns)
{
	(void)sandbox;
	(void)ns;
	return luaext_timers_unavailable("setCpuLimit");
}

bool luaext_timers_set_wall_limit(luaext_sandbox *sandbox, uint64_t ns)
{
	(void)sandbox;
	(void)ns;
	return luaext_timers_unavailable("setWallClockLimit");
}

/* -------------------------------------------------------------------------
 * The bracket around every entry into the interpreter
 * ---------------------------------------------------------------------- */

void luaext_timers_enter_lua(luaext_sandbox *sandbox, luaext_watch_frame *frame)
{
	(void)sandbox;
	frame->was_paused = 0;
	frame->prev_allow_pause = 0;
}

void luaext_timers_leave_lua(luaext_sandbox *sandbox, const luaext_watch_frame *frame)
{
	(void)sandbox;
	(void)frame;
}

/* -------------------------------------------------------------------------
 * Pausing
 * ---------------------------------------------------------------------- */

bool luaext_timers_may_pause(const luaext_sandbox *sandbox)
{
	(void)sandbox;
	return false;
}

bool luaext_timers_pause(luaext_sandbox *sandbox, uint8_t mask)
{
	(void)sandbox;
	(void)mask;
	return false;
}

void luaext_timers_resume(luaext_sandbox *sandbox, uint8_t mask)
{
	(void)sandbox;
	(void)mask;
}

void luaext_timers_php_returned(luaext_sandbox *sandbox)
{
	(void)sandbox;
}

/* -------------------------------------------------------------------------
 * Usage
 * ---------------------------------------------------------------------- */

double luaext_timers_cpu_seconds(const luaext_sandbox *sandbox)
{
	(void)sandbox;
	return 0.0;
}

double luaext_timers_wall_seconds(const luaext_sandbox *sandbox)
{
	(void)sandbox;
	return 0.0;
}

/* -------------------------------------------------------------------------
 * Interrupt request
 * ---------------------------------------------------------------------- */

void luaext_timers_request(luaext_sandbox *sandbox, luaext_irq_reason reason)
{
	(void)sandbox;
	(void)reason;
}

void luaext_timers_hook(lua_State *L, lua_Debug *ar)
{
	(void)L;
	(void)ar;
}
