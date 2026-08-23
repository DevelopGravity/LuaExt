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
