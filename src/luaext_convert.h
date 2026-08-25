/*
 * luaext — moving values between PHP and Lua.
 *
 * Both languages have a type the other lacks, and the mismatches are where
 * quiet data corruption lives: table keys that collide only after conversion,
 * integers that survive in one direction but not the other, and graphs that
 * are cyclic in either. Every one of those is refused loudly rather than
 * papered over.
 */

#ifndef LUAEXT_CONVERT_H
#define LUAEXT_CONVERT_H

#include "luaext_types.h"

/*
 * Push a PHP value onto the Lua stack.
 *
 * null/bool/int/float/string map directly. Arrays become tables. A
 * LuaFunction belonging to this sandbox becomes the function it references.
 * Everything else — objects, resources, references to either — raises a
 * conversion error, because guessing is worse than refusing.
 *
 * Raises a Lua error on failure (cyclic input, unsupported type, depth
 * exceeded), so callers inside a protected call need no return check. Returns
 * only on success.
 */
void luaext_convert_push_zval(luaext_sandbox *sandbox, lua_State *L, zval *value);

/*
 * Convert the Lua value at `index` into `out`.
 *
 * Returns false and throws a PHP exception on failure. Does not pop.
 *
 * Numbers keep their subtype: lua_isinteger decides, so 2 and 2.0 stay
 * distinct rather than both collapsing to a PHP int the way they did before
 * Lua gained an integer type. A Lua integer outside PHP's range becomes a
 * float, matching what PHP itself does with such literals.
 *
 * Tables become arrays. A table carrying both t[1] and t["1"] is refused:
 * PHP would silently merge them, losing data.
 *
 * Functions become LuaFunction handles. Threads and userdata are refused —
 * there is no PHP-side coroutine surface, and handing out a raw userdata
 * would hand out a pointer.
 */
bool luaext_convert_to_zval(luaext_sandbox *sandbox, lua_State *L, int index, zval *out);

/*
 * As above, but bills the PHP-side bytes produced against the memory limit and
 * reports the total through `billed`.
 *
 * For callers that OWN the resulting zval and will release it at a known point.
 * The bytes a conversion allocates on the PHP side are a second copy the limit
 * would otherwise never see -- lua_Alloc bills the Lua original, nothing bills
 * the duplicate. Charging it is only honest where something discharges it, so
 * this variant exists rather than making every conversion bill: a value handed
 * onward to PHP has a lifetime this extension does not control, and charging
 * for it would shrink the effective limit on every call, permanently.
 *
 * The caller MUST pass `*billed` to luaext_alloc_discharge() when it releases
 * the value. `*billed` is meaningful even when the call fails -- a partial
 * conversion has already charged for what it built -- so discharge it either
 * way.
 */
bool luaext_convert_to_zval_billed(luaext_sandbox *sandbox, lua_State *L, int index, zval *out,
								   size_t *billed);

/*
 * Convert `count` values starting at `first` into a zero-indexed PHP array,
 * which is how every multi-return crosses the boundary.
 *
 * Returns false with a thrown exception if any value fails to convert; `out`
 * is left initialised as an empty array in that case.
 */
bool luaext_convert_stack_to_array(luaext_sandbox *sandbox, lua_State *L, int first, int count,
								   zval *out);

/*
 * Reserve a registry slot for the value at `index` and return its id, or -1
 * with a thrown exception if the registry is exhausted.
 *
 * Slots come from the sandbox's freelist so a long-lived worker sandbox does
 * not walk its counter upward forever.
 */
int luaext_convert_ref_create(luaext_sandbox *sandbox, lua_State *L, int index);

/* Push the value a slot holds. Pushes nil if the slot is stale. */
void luaext_convert_ref_push(luaext_sandbox *sandbox, lua_State *L, int ref);

/* Return a slot to the freelist. Safe to call with -1. */
void luaext_convert_ref_release(luaext_sandbox *sandbox, lua_State *L, int ref);

#endif /* LUAEXT_CONVERT_H */
