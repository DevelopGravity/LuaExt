/*
 * luaext — shared data structures.
 *
 * This header is the contract between the subsystems (allocator, conversion,
 * errors, watchdog, policy, VFS, modules). It declares types and constants
 * only; no subsystem logic and no platform headers belong here.
 */

#ifndef LUAEXT_TYPES_H
#define LUAEXT_TYPES_H

#include <php.h>
#include <zend_smart_str.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lua.h"

/*
 * luaext_irq, LUAEXT_IRQ() and LUAEXT_CHECK() come from the vendored Lua tree
 * so the patched interpreter loops can test for an interrupt without ever
 * including a PHP header.
 */
#include "luaext_lua_hooks.h"

#include "php_luaext.h"

/* Opaque to everything but their owning subsystem. */
typedef struct luaext_watch_slot luaext_watch_slot;
typedef struct luaext_vfs luaext_vfs;
typedef struct luaext_modules luaext_modules;
typedef struct luaext_profiler luaext_profiler;

/* -------------------------------------------------------------------------
 * Registry keys
 *
 * The address of each sentinel is the key (lua_pushlightuserdata). Addresses
 * rather than strings: cheaper to look up, and nothing reachable from Lua can
 * name them even if the registry were ever exposed. Defined in luaext_sandbox.c.
 * ---------------------------------------------------------------------- */

extern const char luaext_key_refs;	  /* int -> Lua value, backing PHP handles */
extern const char luaext_key_errmt;	  /* metatable of the fatal-error userdata */
extern const char luaext_key_filemt;  /* metatable of VFS file handles */
extern const char luaext_key_threads; /* weak: thread -> tracking sentinel */
extern const char luaext_key_loaded;  /* package.loaded */
extern const char luaext_key_preload; /* package.preload */
extern const char luaext_key_loading; /* in-flight requires, for cycle detection */
extern const char luaext_key_zvalmt;  /* metatable of zval-holding userdata */

/* -------------------------------------------------------------------------
 * Interrupt reasons
 *
 * Stored in luaext_irq::reason, which lives in the vendored hook header so the
 * patched interpreter can read it without a PHP include.
 *
 * Ordering contract for whoever sets an interrupt (the watchdog, and
 * Sandbox::interrupt()): store the reason with memory_order_relaxed FIRST,
 * then store the flag with memory_order_release. LUAEXT_CHECK loads the flag
 * relaxed on the hot path and executes an acquire fence before acting, so a
 * reader that observes the flag also observes the matching reason. Storing
 * them in the other order, or with relaxed ordering on the flag, lets a
 * weakly-ordered CPU report the wrong reason.
 * ---------------------------------------------------------------------- */

typedef enum {
	LUAEXT_IRQ_NONE = 0,
	LUAEXT_IRQ_CPU,
	LUAEXT_IRQ_WALL,
	LUAEXT_IRQ_OUTPUT,
	LUAEXT_IRQ_ABORT
} luaext_irq_reason;

/* -------------------------------------------------------------------------
 * Errors
 * ---------------------------------------------------------------------- */

typedef enum {
	LUAEXT_ERR_RUNTIME = 0,
	LUAEXT_ERR_SYNTAX,
	LUAEXT_ERR_MEMORY,
	LUAEXT_ERR_CPU,
	LUAEXT_ERR_WALL,
	LUAEXT_ERR_OUTPUT,
	LUAEXT_ERR_COROUTINE,
	LUAEXT_ERR_ABORT,
	LUAEXT_ERR_HANDLER,
	LUAEXT_ERR_PANIC,
	LUAEXT_ERR_CONVERSION,
	LUAEXT_ERR_VFS,
	LUAEXT_ERR_MODULE
} luaext_err_kind;

/*
 * The error value Lua sees. A full userdata rather than a table or string:
 * Lua code cannot construct userdata, so a script cannot forge a fatal error
 * and cannot strip the marker to make a limit breach look catchable.
 * The metatable is private and sets __metatable, so getmetatable() cannot
 * reach it. Uservalue 1 holds the structured traceback.
 */
typedef struct {
	uint32_t magic;
	uint8_t kind; /* luaext_err_kind */
	bool fatal;
	zend_string *message;

	/*
	 * The exception a PHP callback threw, kept alive so it can be rethrown to
	 * the host unchanged rather than degraded to its message text.
	 */
	zval php_exception;
} luaext_error_ud;

#define LUAEXT_ERROR_MAGIC 0x4C584572u /* "LXEr" */

/* -------------------------------------------------------------------------
 * Capabilities
 *
 * Mirrors DevelopGravity\LuaExt\Capabilities. Resolved once at construction:
 * anything that shapes the lua_State cannot change afterwards.
 * ---------------------------------------------------------------------- */

typedef enum {
	LUAEXT_CAP_LOAD_BYTECODE = 1u << 0,
	LUAEXT_CAP_COMPILE_AT_RUNTIME = 1u << 1, /* Lua-visible load() */
	LUAEXT_CAP_DUMP_BYTECODE = 1u << 2,
	LUAEXT_CAP_REQUIRE = 1u << 3,
	LUAEXT_CAP_VFS = 1u << 4,
	LUAEXT_CAP_VFS_WRITE = 1u << 5,
	LUAEXT_CAP_COROUTINES = 1u << 6,
	LUAEXT_CAP_OS_TIME = 1u << 7,
	LUAEXT_CAP_OS_ENV = 1u << 8,
	LUAEXT_CAP_DEBUG_TRACEBACK = 1u << 9,
	LUAEXT_CAP_DEBUG_INTROSPECT = 1u << 10, /* getinfo/getlocal/getupvalue */
	LUAEXT_CAP_DEBUG_MUTATE = 1u << 11,		/* setlocal/setupvalue/getregistry */
	LUAEXT_CAP_DEBUG_HOOKS = 1u << 12,		/* debug.sethook: defeats the watchdog */
	LUAEXT_CAP_UTF8 = 1u << 13,
	LUAEXT_CAP_GC_CONTROL = 1u << 14,
	LUAEXT_CAP_WARN = 1u << 15
} luaext_cap;

#define LUAEXT_CAPS_UNTRUSTED                                                                      \
	(LUAEXT_CAP_COROUTINES | LUAEXT_CAP_OS_TIME | LUAEXT_CAP_DEBUG_TRACEBACK | LUAEXT_CAP_UTF8)

/*
 * Bytecode loading, debug mutation and debug hooks stay off even here: each
 * one voids a guarantee the sandbox otherwise makes, so enabling them has to
 * be a deliberate per-flag decision rather than a side effect of "trusted".
 */
#define LUAEXT_CAPS_TRUSTED                                                                        \
	(LUAEXT_CAPS_UNTRUSTED | LUAEXT_CAP_COMPILE_AT_RUNTIME | LUAEXT_CAP_DUMP_BYTECODE |            \
	 LUAEXT_CAP_REQUIRE | LUAEXT_CAP_VFS | LUAEXT_CAP_DEBUG_INTROSPECT | LUAEXT_CAP_GC_CONTROL |   \
	 LUAEXT_CAP_WARN)

/*
 * LUAEXT_CAP_VFS_WRITE is deliberately NOT in the list above, and it used to be.
 *
 * Trusting a script to read the host's store is a different decision from
 * trusting it to modify it, and a preset is the wrong place to make the second
 * one: "trusted" describes the code's provenance, not whether this particular
 * sandbox should be able to overwrite anything the backend exposes. Rolling
 * write access into the preset meant every host that reached for trusted() got
 * it without ever typing the word, which is the same shape as the LUAEXT_LIB_*
 * confusions warned about below -- a capability satisfied as a side effect of a
 * coarser one.
 *
 * A host that wants it asks for it: Capabilities::trusted()->with(vfsWrite: true).
 */

/*
 * Which standard libraries to open wholesale; mirrors Lua's own LUA_*LIBK bits.
 *
 * A bit here means "install the upstream library exactly as shipped". That is
 * NOT the same claim as "the matching capability is granted", and conflating
 * the two is a privilege escalation:
 *
 *   LUAEXT_LIB_DEBUG must stay clear even when debugTraceback is granted.
 *   debugTraceback is part of the untrusted baseline, so mapping it to this bit
 *   would hand every untrusted sandbox the whole debug library — including
 *   debug.sethook, which displaces the hook the CPU limit depends on, and
 *   debug.setupvalue, which walks straight out of the sandbox. Only a filtered
 *   table built by the library policy may satisfy that capability.
 *
 *   LUAEXT_LIB_CORO has the same trap. The coroutine library is only ever
 *   installed through our own wrapper, which caps live coroutines and stops
 *   resume from swallowing a fatal error; upstream luaopen_coroutine does
 *   neither.
 *
 * Set a bit only when opening the upstream library unmodified is genuinely
 * safe for the least-privileged sandbox that can reach it.
 */
typedef enum {
	LUAEXT_LIB_BASE = 1u << 0,
	LUAEXT_LIB_CORO = 1u << 1,
	LUAEXT_LIB_TABLE = 1u << 2,
	LUAEXT_LIB_STR = 1u << 3,
	LUAEXT_LIB_MATH = 1u << 4,
	LUAEXT_LIB_UTF8 = 1u << 5,
	LUAEXT_LIB_DEBUG = 1u << 6
} luaext_lib;

/* -------------------------------------------------------------------------
 * Limits and quotas
 * ---------------------------------------------------------------------- */

typedef enum { LUAEXT_OVERFLOW_TRUNCATE = 0, LUAEXT_OVERFLOW_FAIL } luaext_overflow;

typedef enum {
	LUAEXT_OUTPUT_BUFFER = 0,
	LUAEXT_OUTPUT_CALLBACK,
	LUAEXT_OUTPUT_DISCARD
} luaext_output_mode;

/* How well the running platform can enforce a limit; surfaced by features(). */
typedef enum {
	LUAEXT_LIMIT_ENFORCED = 0,
	LUAEXT_LIMIT_DEGRADED, /* coarse accounting, backed by a wall-clock deadline */
	LUAEXT_LIMIT_UNSUPPORTED
} luaext_limit_support;

/* Throughout: 0 means "no limit". */
/*
 * Largest time limit expressible in nanoseconds, in seconds (~584 years).
 *
 * Every path that turns host-supplied seconds into nanoseconds checks this
 * BEFORE the cast, because converting an out-of-range double to an integer is
 * undefined behaviour and the value it lands on can be zero -- which is how
 * this API spells "no ceiling". A limit becoming its own absence is the exact
 * failure this extension exists to eliminate, so the bound lives here rather
 * than being spelled out again at each site.
 *
 * DERIVED, not written down. This is a property of the type the deadline is
 * stored in and nothing else: it is the same on every platform and every
 * processor, and it follows the field automatically if that type ever changes.
 * What does vary between machines is the clock's RESOLUTION, which is a
 * different quantity entirely and is reported through
 * Sandbox::features()['cpuResolutionSeconds'].
 *
 * Integer division on purpose. (double)UINT64_MAX rounds UP to 2^64, so a
 * bound derived from it would admit a value whose product with 1e9 lands
 * exactly on 2^64 -- back to the undefined conversion this exists to prevent.
 * Truncating first leaves most of a second of headroom, which costs nothing at
 * a ceiling of nearly six centuries.
 */
#define LUAEXT_LIMIT_MAX_SECONDS ((double)(UINT64_MAX / UINT64_C(1000000000)))

typedef struct {
	size_t memory_bytes;
	uint64_t cpu_ns;
	uint64_t wall_ns;

	size_t output_bytes;
	uint8_t output_overflow; /* luaext_overflow */

	uint32_t max_live_coroutines;
	uint32_t max_coroutine_depth;
	uint32_t max_call_depth;

	uint32_t max_modules;
	uint32_t max_require_depth;

	size_t max_string_length;

	/*
	 * The parser runs before any interrupt hook can fire, so pathological
	 * sources are bounded by length rather than by the CPU limit.
	 */
	size_t max_source_bytes;

	uint32_t max_conversion_depth;
} luaext_limits;

typedef struct {
	uint32_t max_open_handles;
	size_t max_file_bytes;
	size_t max_total_bytes;
	uint32_t max_files;
	uint32_t max_operations;
	uint32_t max_path_length;
	uint32_t max_path_depth;

	/* Charge time spent inside the host backend to the wall-clock deadline. */
	bool bill_wall_time;
} luaext_vfs_quota;

typedef struct {
	uint32_t caps;		/* luaext_cap bitset */
	uint32_t open_libs; /* luaext_lib bitset */
	luaext_limits limits;
	luaext_vfs_quota vfs_quota;

	/*
	 * The seed for everything a script could otherwise use to learn about the
	 * host's address space: the string hash, and the random generator the math
	 * library is opened with.
	 *
	 * `seed_is_fixed` records that the host asked for a specific one. It is not
	 * merely informational -- surrendering hash-flood protection has to be a
	 * deliberate act, so SandboxConfig refuses a fixed seed unless the host also
	 * passes deterministic: true, and this is what carries that decision far
	 * enough to be acted on.
	 */
	uint64_t seed;
	bool seed_is_fixed;
} luaext_policy;

#define luaext_has_cap(policy, cap) (((policy)->caps & (uint32_t)(cap)) != 0)

/* -------------------------------------------------------------------------
 * Memory accounting
 *
 * Every Lua allocation passes through luaext_alloc. `charged` covers bytes the
 * sandbox holds outside the Lua heap (VFS buffers, the output buffer): they are
 * invisible to lua_Alloc but are still the script's doing, so they count against
 * the same budget. usage + charged is what the limit applies to.
 * ---------------------------------------------------------------------- */

typedef struct {
	size_t limit;
	size_t usage;
	size_t charged;
	size_t peak;
	size_t gc_last_tune;
} luaext_alloc;

/* -------------------------------------------------------------------------
 * Output sink
 * ---------------------------------------------------------------------- */

typedef struct {
	uint8_t mode; /* luaext_output_mode */
	smart_str buf;
	zval callback;
	size_t written;
	size_t limit;
	size_t chunk;
	bool truncated;
} luaext_output;

/* -------------------------------------------------------------------------
 * Deferred releases
 *
 * PHP references a Lua finaliser must not drop where it stands, because doing
 * so can run a __destruct that re-enters the collecting state. See
 * luaext_defer.h.
 * ---------------------------------------------------------------------- */

typedef enum {
	LUAEXT_DEFER_ZVAL,
	LUAEXT_DEFER_FCC,
} luaext_defer_kind;

typedef struct {
	luaext_defer_kind kind;

	union {
		zval value;
		zend_fcall_info_cache fcc;
	} as;
} luaext_defer_item;

typedef struct {
	luaext_defer_item *items; /* pemalloc'd; __gc outlives the request allocator */
	size_t count;
	size_t capacity;
} luaext_deferred;

/* -------------------------------------------------------------------------
 * Sandbox
 * ---------------------------------------------------------------------- */

struct luaext_sandbox {
	/*
	 * Must stay first: every lua_State's extra space holds a luaext_sandbox *,
	 * and the patched interpreter reads it as a luaext_irq * (see
	 * luaext_lua_hooks.h) without knowing this struct's layout.
	 */
	luaext_irq irq;

	lua_State *L;
	uint64_t seed;
	luaext_alloc alloc;
	luaext_policy policy;

	/* Non-NULL only while a limit is armed. Shared with the watchdog thread. */
	luaext_watch_slot *slot;

	/* The state currently executing, tracked so a resume interrupts the
	 * innermost coroutine rather than the main thread. */
	lua_State *running_L;

	int in_lua; /* nesting depth of calls into Lua */
	int in_php; /* nesting depth of callbacks out to PHP */

	/*
	 * Raising a Lua error longjmps past C cleanup, so it is only safe where no
	 * frame holds an unreleased zval. Debug builds assert this is zero before
	 * raising.
	 */
	int no_raise_depth;

	uint32_t co_live;
	uint32_t co_depth;
	uint32_t co_peak_depth;

	/* PHP references waiting to be released somewhere the collector is not
	 * running. See luaext_defer.h. */
	luaext_deferred deferred;

	bool allow_pause;
	bool closed;
	bool panicked;

	/* Captured at construction; other threads may only call interrupt(). */
	uintptr_t owner_thread;

	luaext_output out;
	luaext_vfs *vfs;
	luaext_modules *modules;
	luaext_profiler *profiler;

	/* Keeps the FileSystem, ModuleResolver and output callback alive. */
	zval config_zv;

	/*
	 * Slots in the registry refs table. Released slots are reused: a
	 * long-lived worker sandbox would otherwise walk next_ref to INT_MAX.
	 */
	int next_ref;
	int *ref_freelist;
	uint32_t ref_freelist_len;
	uint32_t ref_freelist_cap;

	/* Reported by stats(). */
	uint64_t lua_calls_in;
	uint64_t php_calls_out;
	uint64_t gc_collections;
	uint64_t modules_loaded;
	uint64_t vfs_operations;
	uint64_t vfs_bytes;

	/* Per-thread live list in module globals, swept at RSHUTDOWN. */
	luaext_sandbox *live_next;
	luaext_sandbox *live_prev;

	zend_object std; /* must stay last */
};

_Static_assert(offsetof(struct luaext_sandbox, irq) == 0,
			   "luaext_irq must be the first member: the vendored Lua hooks cast the "
			   "extra-space pointer straight to luaext_irq *");

static zend_always_inline luaext_sandbox *luaext_sandbox_from_obj(zend_object *obj)
{
	return (luaext_sandbox *)((char *)obj - XtOffsetOf(struct luaext_sandbox, std));
}

#define Z_LUAEXT_SANDBOX_P(zv) luaext_sandbox_from_obj(Z_OBJ_P(zv))

/* Reads the owning sandbox out of any state's extra space, main or coroutine. */
#define LUAEXT_SB(L) (*(luaext_sandbox **)lua_getextraspace(L))

/* -------------------------------------------------------------------------
 * LuaFunction handle
 * ---------------------------------------------------------------------- */

typedef struct {
	zval sandbox_zv;
	int ref; /* slot in the registry refs table, or -1 once invalidated */
	zend_object std;
} luaext_function_obj;

static zend_always_inline luaext_function_obj *luaext_function_from_obj(zend_object *obj)
{
	return (luaext_function_obj *)((char *)obj - XtOffsetOf(luaext_function_obj, std));
}

#define Z_LUAEXT_FUNCTION_P(zv) luaext_function_from_obj(Z_OBJ_P(zv))

#endif /* LUAEXT_TYPES_H */
