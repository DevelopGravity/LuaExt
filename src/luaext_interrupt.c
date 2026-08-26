/*
 * luaext — interrupt delivery into the interpreter.
 *
 * The patched loops in the vendored Lua tree call LUAEXT_CHECK(), which lands
 * here once something has raised the interrupt flag. The declaration lives in
 * third_party/lua-5.5.1/src/luaext_lua_hooks.h, which deliberately knows nothing
 * about PHP; this is the only definition in the extension.
 */

#include "luaext_types.h"

#include "luaext_error.h"

#include <lua.h>

/*
 * Stop the running script.
 *
 * Two things about this function are load-bearing.
 *
 * The first is that it raises the unforgeable fatal-error userdata rather than a
 * string. luaL_error() would produce a value any pcall could catch, and a limit
 * a script can catch is not a limit. The error subsystem's userdata is marked
 * fatal, so the sandbox's own pcall replacement re-raises it and the host
 * receives the matching typed exception.
 *
 * The second is that the flag is NOT cleared here. It stays set until the
 * outermost call disarms, and it has to: Lua itself swallows errors in places
 * the sandbox cannot patch away -- GCTM turns an error raised inside a __gc
 * finaliser into a warning -- and in those places the still-pending flag is the
 * only thing that stops the script at the next instruction. Clearing it on raise
 * would make "catch it by dying in a finaliser" a working escape.
 */
/*
 * Whether an interrupt is pending, WITHOUT raising.
 *
 * The companion to luaext_raise_interrupt, and it exists because the Lua -> PHP
 * direction may not raise. Conversion runs with PHP zvals in hand and, in the
 * callback direction, underneath a C frame that PHP is about to return through:
 * a longjmp from there strands whatever the converter has built. So the loops
 * ask, and unwind through their own failure path instead.
 *
 * Relaxed like the hot-path check, and for the same reason -- it only answers
 * "is anything pending?". The caller is not acting on the reason, only stopping.
 */
bool luaext_interrupt_pending(lua_State *L)
{
	const luaext_irq *queue = LUAEXT_IRQ(L);

	return queue != NULL && atomic_load_explicit(&queue->interrupted, memory_order_relaxed) != 0;
}

void luaext_raise_interrupt(lua_State *L)
{
	luaext_sandbox *sandbox = LUAEXT_SB(L);
	unsigned char reason = (unsigned char)LUAEXT_IRQ_ABORT;

	if (sandbox != NULL) {
		reason = atomic_load_explicit(&sandbox->irq.reason, memory_order_relaxed);
	}

	switch ((luaext_irq_reason)reason) {
	case LUAEXT_IRQ_CPU:
		luaext_error_raise(L, LUAEXT_ERR_CPU, true,
						   "The script exceeded its CPU limit and was stopped");
		break;

	case LUAEXT_IRQ_WALL:
		luaext_error_raise(L, LUAEXT_ERR_WALL, true,
						   "The script exceeded its wall-clock limit and was stopped");
		break;

	case LUAEXT_IRQ_OUTPUT:
		luaext_error_raise(L, LUAEXT_ERR_OUTPUT, true,
						   "The script exceeded its output limit and was stopped");
		break;

	case LUAEXT_IRQ_NONE:
	case LUAEXT_IRQ_ABORT:
	default:
		/*
		 * NONE lands here too. It should be unreachable -- the writer stores the
		 * reason before the flag -- but "the reason was somehow not there" must
		 * still stop the script, and a host abort is the honest description of
		 * an interrupt whose origin cannot be named.
		 */
		luaext_error_raise(L, LUAEXT_ERR_ABORT, true,
						   "The script was interrupted by the host and was stopped");
		break;
	}
}
