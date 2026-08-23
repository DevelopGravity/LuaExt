/*
 * luaext — a sandbox for running untrusted Lua 5.5 inside PHP.
 *
 * Module-level declarations: entry point, class entries, per-thread globals.
 * Data structures live in luaext_types.h.
 */

#ifndef PHP_LUAEXT_H
#define PHP_LUAEXT_H

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <php.h>

#define PHP_LUAEXT_NAME "luaext"

/*
 * Release tags are plain semver with no "v" prefix; the release workflow
 * refuses to publish when this macro and the tag disagree.
 */
#define PHP_LUAEXT_VERSION "0.1.0-dev"

extern zend_module_entry luaext_module_entry;
#define phpext_luaext_ptr &luaext_module_entry

#if defined(ZTS) && defined(COMPILE_DL_LUAEXT)
ZEND_TSRMLS_CACHE_EXTERN()
#endif

typedef struct luaext_sandbox luaext_sandbox;

/* Core objects. */
extern zend_class_entry *luaext_ce_sandbox;
extern zend_class_entry *luaext_ce_lua_function;

/* Configuration value objects. */
extern zend_class_entry *luaext_ce_sandbox_config;
extern zend_class_entry *luaext_ce_capabilities;
extern zend_class_entry *luaext_ce_limits;
extern zend_class_entry *luaext_ce_vfs_quota;
extern zend_class_entry *luaext_ce_sandbox_stats;

/* Host integration contracts. */
extern zend_class_entry *luaext_ce_file_system;
extern zend_class_entry *luaext_ce_ranged_file_system;
extern zend_class_entry *luaext_ce_file_stat;
extern zend_class_entry *luaext_ce_module_resolver;
extern zend_class_entry *luaext_ce_module_source;
extern zend_class_entry *luaext_ce_lua_method_attribute;

/* Enums. */
extern zend_class_entry *luaext_ce_output_mode;
extern zend_class_entry *luaext_ce_overflow_behavior;
extern zend_class_entry *luaext_ce_profiler_unit;
extern zend_class_entry *luaext_ce_interrupt_reason;
extern zend_class_entry *luaext_ce_limit_support;

/*
 * Exceptions. RuntimeError and its subclasses are the only ones a Lua script
 * may catch with pcall; every FatalError subclass is re-raised through the
 * sandbox's pcall replacement so a script cannot swallow its own limits.
 */
extern zend_class_entry *luaext_ce_lua_throwable;
extern zend_class_entry *luaext_ce_lua_exception;
extern zend_class_entry *luaext_ce_runtime_error;
extern zend_class_entry *luaext_ce_vfs_error;
extern zend_class_entry *luaext_ce_module_not_found_error;
extern zend_class_entry *luaext_ce_fatal_error;
extern zend_class_entry *luaext_ce_syntax_error;
extern zend_class_entry *luaext_ce_memory_limit_error;
extern zend_class_entry *luaext_ce_cpu_limit_error;
extern zend_class_entry *luaext_ce_wall_clock_limit_error;
extern zend_class_entry *luaext_ce_output_limit_error;
extern zend_class_entry *luaext_ce_coroutine_limit_error;
extern zend_class_entry *luaext_ce_host_abort_error;
extern zend_class_entry *luaext_ce_error_handler_error;
extern zend_class_entry *luaext_ce_panic_error;
extern zend_class_entry *luaext_ce_conversion_error;

/* Host misuse: never crosses into Lua. */
extern zend_class_entry *luaext_ce_configuration_error;
extern zend_class_entry *luaext_ce_capability_error;
extern zend_class_entry *luaext_ce_closed_sandbox_error;
extern zend_class_entry *luaext_ce_thread_affinity_error;

ZEND_BEGIN_MODULE_GLOBALS(luaext)
/*
	 * Sandboxes constructed on this thread, newest first, linked through
	 * luaext_sandbox::live_next/live_prev. RSHUTDOWN walks the list and closes
	 * whatever the host left open, so a request cannot leak a Lua heap.
	 */
luaext_sandbox *live_sandboxes;
zend_long live_count;

/* Instruction interval for the always-armed interrupt hook; 0 disables it. */
zend_long hook_count;

/* Floor on watchdog wake-ups, bounding worst-case limit overshoot. */
zend_long watchdog_resolution_us;

/*
	 * Route Lua allocations through the Zend allocator instead of malloc.
	 * Off by default: a sandbox may outlive the request that built it in a
	 * worker SAPI, and request-local memory would be freed underneath it.
	 */
bool use_zend_mm;
ZEND_END_MODULE_GLOBALS(luaext)

ZEND_EXTERN_MODULE_GLOBALS(luaext)

#define LUAEXT_G(v) ZEND_MODULE_GLOBALS_ACCESSOR(luaext, v)

#endif /* PHP_LUAEXT_H */
