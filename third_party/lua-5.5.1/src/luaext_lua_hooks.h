#ifndef LUAEXT_LUA_HOOKS_H
#define LUAEXT_LUA_HOOKS_H
#include <stdatomic.h>
/* The extension's sandbox struct begins with this prefix; a pointer to it
   is stored in every lua_State's extra space (set right after lua_newstate,
   inherited by coroutines via lua_newthread). */
typedef struct luaext_irq {
    atomic_uchar interrupted;
    atomic_uchar reason;
} luaext_irq;
struct lua_State;
/* Defined by the extension (src/luaext_interrupt.c). Does not return. */
extern void luaext_raise_interrupt(struct lua_State *L);
/* Non-zero when the state's current debug hook is the extension's own count
   hook. Defined by the extension (src/luaext_timers.c). 'lgc.c' asks before
   deciding whether to leave hooks enabled around a __gc finalizer: upstream
   disables them there because an arbitrary Lua hook function may allocate,
   yield or re-enter the collector, and ours -- a C function that allocates
   nothing and either returns or raises -- does none of those. */
extern int luaext_hook_is_ours(struct lua_State *L);
/* The capability whose absence explains why the named GLOBAL is nil, or NULL
   when the name is not one this sandbox withholds. Defined by the extension
   (src/luaext_openlibs.c). 'ldebug.c' asks while formatting an
   attempt-to-index/call error; NULL keeps upstream's message byte-for-byte. */
extern const char *luaext_withheld_capability(struct lua_State *L, const char *name);
/* Raise the extension's own error for an access luaext_withheld_capability()
   classified. Longjmps exactly where luaG_runerror() would two lines later;
   does not return. Defined by the extension (src/luaext_error.c). */
extern void luaext_raise_withheld(struct lua_State *L, const char *name,
                                  const char *capability);
#define LUAEXT_IRQ(L) (*(luaext_irq **)lua_getextraspace(L))
/* The hot-path load is relaxed: it only answers "is anything pending?", and
   costs nothing when nothing is. Ordering is established on the slow path
   instead -- the acquire fence below pairs with the writer's release store,
   so a reader that observes the flag also observes the reason written before
   it. Writers MUST store reason first (relaxed), then the flag with
   memory_order_release. */
#define LUAEXT_CHECK(L) \
  do { luaext_irq *luaext_q_ = LUAEXT_IRQ(L); \
       if (luaext_q_ != NULL && \
           atomic_load_explicit(&luaext_q_->interrupted, memory_order_relaxed)) { \
         atomic_thread_fence(memory_order_acquire); \
         luaext_raise_interrupt(L); } } while (0)
#endif
