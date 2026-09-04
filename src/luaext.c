/*
 * luaext — module entry point.
 *
 * Owns everything that is per-process or per-request: the class registrations
 * generated from the stub sources, the INI settings backing the module globals,
 * and the request-shutdown sweep that closes any sandbox the host left open.
 */

#include "php_luaext.h"

#include "luaext_config.h"
#include "luaext_function.h"
#include "luaext_sandbox.h"
#include "luaext_seal.h"
#include "luaext_timers.h"
#include "luaext_types.h"

#include <lua.h>

#include <Zend/zend_attributes.h>
#include <Zend/zend_enum.h>
#include <Zend/zend_exceptions.h>
#include <ext/json/php_json.h>
#include <ext/spl/spl_exceptions.h>
#include <ext/standard/info.h>

ZEND_DECLARE_MODULE_GLOBALS(luaext)

/* Generated from stubs/; regenerate with tools/gen-stubs.sh. */
#include "luaext_arginfo.h"
#include "luaext_exceptions_arginfo.h"

/* -------------------------------------------------------------------------
 * Class entries
 * ---------------------------------------------------------------------- */

zend_class_entry *luaext_ce_sandbox;
zend_class_entry *luaext_ce_lua_function;

zend_class_entry *luaext_ce_sandbox_config;
zend_class_entry *luaext_ce_capabilities;
zend_class_entry *luaext_ce_limits;
zend_class_entry *luaext_ce_vfs_quota;
zend_class_entry *luaext_ce_sandbox_stats;
zend_class_entry *luaext_ce_validation_result;

zend_class_entry *luaext_ce_file_system;
zend_class_entry *luaext_ce_ranged_file_system;
zend_class_entry *luaext_ce_file_stat;
zend_class_entry *luaext_ce_module_resolver;
zend_class_entry *luaext_ce_module_source;
zend_class_entry *luaext_ce_lua_method_attribute;

zend_class_entry *luaext_ce_output_mode;
zend_class_entry *luaext_ce_overflow_behavior;
zend_class_entry *luaext_ce_profiler_unit;
zend_class_entry *luaext_ce_limit_support;
zend_class_entry *luaext_ce_seal_mode;

zend_class_entry *luaext_ce_lua_throwable;
zend_class_entry *luaext_ce_lua_exception;
zend_class_entry *luaext_ce_runtime_error;
zend_class_entry *luaext_ce_vfs_error;
zend_class_entry *luaext_ce_module_not_found_error;
zend_class_entry *luaext_ce_fatal_error;
zend_class_entry *luaext_ce_syntax_error;
zend_class_entry *luaext_ce_source_limit_error;
zend_class_entry *luaext_ce_bytecode_integrity_error;
zend_class_entry *luaext_ce_memory_limit_error;
zend_class_entry *luaext_ce_cpu_limit_error;
zend_class_entry *luaext_ce_wall_clock_limit_error;
zend_class_entry *luaext_ce_output_limit_error;
zend_class_entry *luaext_ce_coroutine_limit_error;
zend_class_entry *luaext_ce_host_abort_error;
zend_class_entry *luaext_ce_error_handler_error;
zend_class_entry *luaext_ce_panic_error;
zend_class_entry *luaext_ce_conversion_error;

zend_class_entry *luaext_ce_configuration_error;
zend_class_entry *luaext_ce_capability_error;
zend_class_entry *luaext_ce_closed_sandbox_error;
zend_class_entry *luaext_ce_thread_affinity_error;

/*
 * LuaLogicException is the shared base of the host-misuse exceptions. It is not
 * in php_luaext.h because nothing outside this file needs it: the extension
 * always throws one of its concrete subclasses.
 */
static zend_class_entry *luaext_ce_lua_logic_exception;

/* -------------------------------------------------------------------------
 * INI
 * ---------------------------------------------------------------------- */

PHP_INI_BEGIN()
/*
	 * Instruction interval of the always-armed interrupt hook. Lower reacts to a
	 * limit sooner, higher costs less per instruction; 0 disables the hook, which
	 * also disables the only race-free half of interrupt delivery.
	 */
STD_PHP_INI_ENTRY("luaext.hook_count", "1000", PHP_INI_ALL, OnUpdateLong, hook_count,
				  zend_luaext_globals, luaext_globals)

/*
	 * Floor on watchdog wake-ups, in microseconds. It bounds how far past its
	 * budget a script can run, and how much a mostly-idle process pays for the
	 * watchdog thread.
	 */
STD_PHP_INI_ENTRY("luaext.watchdog_resolution_us", "500", PHP_INI_SYSTEM, OnUpdateLong,
				  watchdog_resolution_us, zend_luaext_globals, luaext_globals)

/*
	 * Whether an UNSEALED binary chunk may be loaded at all, by compileBinary()
	 * or by a script's own load(..., "b").
	 *
	 * Off by default, and that is the security posture rather than a
	 * preference: Lua's loader checks the header and stops, so a corrupted
	 * instruction stream reaches the VM intact. Measured by flipping one byte at
	 * each position of a small chunk -- 57% refused, 33% ran anyway, 10% killed
	 * the process. Sealed blobs carry an HMAC and are always allowed; this
	 * reopens the path for blobs nothing can vouch for.
	 */
STD_PHP_INI_BOOLEAN("luaext.allow_raw_bytecode", "0", PHP_INI_SYSTEM, OnUpdateBool,
					allow_raw_bytecode, zend_luaext_globals, luaext_globals)

/*
	 * Benchmarking switch only. A sandbox may outlive the request that built it
	 * in a worker SAPI, and request-local memory would be freed underneath it.
	 */
STD_PHP_INI_BOOLEAN("luaext.use_zend_mm", "0", PHP_INI_SYSTEM, OnUpdateBool, use_zend_mm,
					zend_luaext_globals, luaext_globals)
PHP_INI_END()

/* -------------------------------------------------------------------------
 * Module globals
 * ---------------------------------------------------------------------- */

static PHP_GINIT_FUNCTION(luaext)
{
#if defined(COMPILE_DL_LUAEXT) && defined(ZTS)
	ZEND_TSRMLS_CACHE_UPDATE();
#endif

	memset(luaext_globals, 0, sizeof(*luaext_globals));
}

static PHP_GSHUTDOWN_FUNCTION(luaext)
{
	/*
	 * The live list is emptied by the RSHUTDOWN sweep. Reaching here with
	 * entries left would mean a sandbox outlived its thread, which the thread
	 * affinity rule forbids.
	 */
	LUAEXT_ASSERT(luaext_globals->live_sandboxes == NULL);
}

/* -------------------------------------------------------------------------
 * Startup and shutdown
 * ---------------------------------------------------------------------- */

static void luaext_register_classes(void)
{
	luaext_ce_output_mode = register_class_DevelopGravity_LuaExt_OutputMode();
	luaext_ce_overflow_behavior = register_class_DevelopGravity_LuaExt_OverflowBehavior();
	luaext_ce_profiler_unit = register_class_DevelopGravity_LuaExt_ProfilerUnit();
	luaext_ce_limit_support = register_class_DevelopGravity_LuaExt_LimitSupport();
	luaext_ce_seal_mode = register_class_DevelopGravity_LuaExt_SealMode();

	luaext_ce_lua_method_attribute = register_class_DevelopGravity_LuaExt_LuaMethod();

	/*
	 * The stub carries no #[Attribute] marker: gen_stub only resolves class
	 * constants it declares itself. Registering it here is what makes
	 * #[LuaMethod] usable on a method, and restricting it to methods is what
	 * keeps registerObject()'s selection rule meaningful.
	 */
	zend_internal_attribute_register(luaext_ce_lua_method_attribute, ZEND_ATTRIBUTE_TARGET_METHOD);

	luaext_ce_capabilities = register_class_DevelopGravity_LuaExt_Capabilities();
	luaext_ce_limits = register_class_DevelopGravity_LuaExt_Limits();
	luaext_ce_vfs_quota = register_class_DevelopGravity_LuaExt_VfsQuota();
	luaext_ce_sandbox_config = register_class_DevelopGravity_LuaExt_SandboxConfig();
	luaext_ce_sandbox_stats =
		register_class_DevelopGravity_LuaExt_SandboxStats(php_json_serializable_ce);
	luaext_ce_validation_result =
		register_class_DevelopGravity_LuaExt_ValidationResult(php_json_serializable_ce);

	luaext_ce_sandbox = register_class_DevelopGravity_LuaExt_Sandbox();
	luaext_ce_lua_function = register_class_DevelopGravity_LuaExt_LuaFunction();

	luaext_ce_file_stat = register_class_DevelopGravity_LuaExt_FileStat();
	luaext_ce_file_system = register_class_DevelopGravity_LuaExt_FileSystem();
	luaext_ce_ranged_file_system =
		register_class_DevelopGravity_LuaExt_RangedFileSystem(luaext_ce_file_system);
	luaext_ce_module_source = register_class_DevelopGravity_LuaExt_ModuleSource();
	luaext_ce_module_resolver = register_class_DevelopGravity_LuaExt_ModuleResolver();
}

static void luaext_register_exceptions(void)
{
	luaext_ce_lua_throwable =
		register_class_DevelopGravity_LuaExt_Exception_LuaThrowable(zend_ce_throwable);

	luaext_ce_lua_exception = register_class_DevelopGravity_LuaExt_Exception_LuaException(
		spl_ce_RuntimeException, luaext_ce_lua_throwable);
	luaext_ce_lua_logic_exception =
		register_class_DevelopGravity_LuaExt_Exception_LuaLogicException(spl_ce_LogicException,
																		 luaext_ce_lua_throwable);

	luaext_ce_runtime_error =
		register_class_DevelopGravity_LuaExt_Exception_RuntimeError(luaext_ce_lua_exception);
	luaext_ce_vfs_error =
		register_class_DevelopGravity_LuaExt_Exception_VfsError(luaext_ce_runtime_error);
	luaext_ce_module_not_found_error =
		register_class_DevelopGravity_LuaExt_Exception_ModuleNotFoundError(luaext_ce_runtime_error);

	luaext_ce_fatal_error =
		register_class_DevelopGravity_LuaExt_Exception_FatalError(luaext_ce_lua_exception);
	luaext_ce_syntax_error =
		register_class_DevelopGravity_LuaExt_Exception_SyntaxError(luaext_ce_fatal_error);
	luaext_ce_source_limit_error =
		register_class_DevelopGravity_LuaExt_Exception_SourceLimitError(luaext_ce_fatal_error);
	luaext_ce_bytecode_integrity_error =
		register_class_DevelopGravity_LuaExt_Exception_BytecodeIntegrityError(
			luaext_ce_fatal_error);
	luaext_ce_memory_limit_error =
		register_class_DevelopGravity_LuaExt_Exception_MemoryLimitError(luaext_ce_fatal_error);
	luaext_ce_cpu_limit_error =
		register_class_DevelopGravity_LuaExt_Exception_CpuLimitError(luaext_ce_fatal_error);
	luaext_ce_wall_clock_limit_error =
		register_class_DevelopGravity_LuaExt_Exception_WallClockLimitError(luaext_ce_fatal_error);
	luaext_ce_output_limit_error =
		register_class_DevelopGravity_LuaExt_Exception_OutputLimitError(luaext_ce_fatal_error);
	luaext_ce_coroutine_limit_error =
		register_class_DevelopGravity_LuaExt_Exception_CoroutineLimitError(luaext_ce_fatal_error);
	luaext_ce_host_abort_error =
		register_class_DevelopGravity_LuaExt_Exception_HostAbortError(luaext_ce_fatal_error);
	luaext_ce_error_handler_error =
		register_class_DevelopGravity_LuaExt_Exception_ErrorHandlerError(luaext_ce_fatal_error);
	luaext_ce_panic_error =
		register_class_DevelopGravity_LuaExt_Exception_PanicError(luaext_ce_fatal_error);
	luaext_ce_conversion_error =
		register_class_DevelopGravity_LuaExt_Exception_ConversionError(luaext_ce_fatal_error);

	luaext_ce_configuration_error =
		register_class_DevelopGravity_LuaExt_Exception_ConfigurationError(
			luaext_ce_lua_logic_exception);
	luaext_ce_capability_error = register_class_DevelopGravity_LuaExt_Exception_CapabilityError(
		luaext_ce_lua_logic_exception);
	luaext_ce_closed_sandbox_error =
		register_class_DevelopGravity_LuaExt_Exception_ClosedSandboxError(
			luaext_ce_lua_logic_exception);
	luaext_ce_thread_affinity_error =
		register_class_DevelopGravity_LuaExt_Exception_ThreadAffinityError(
			luaext_ce_lua_logic_exception);
}

static PHP_MINIT_FUNCTION(luaext)
{
	REGISTER_INI_ENTRIES();

	luaext_register_classes();
	luaext_register_exceptions();
	luaext_sandbox_startup();
	luaext_function_startup();
	luaext_seal_startup();
	luaext_config_startup();

	/* Probes the platform clocks and prepares the slot pool. Deliberately does
	 * not start the watchdog thread: that is lazy, on the first armed limit, so
	 * a process that never sets one never pays for it. */
	luaext_timers_startup();

	return SUCCESS;
}

static PHP_MSHUTDOWN_FUNCTION(luaext)
{
	/*
	 * Stops and JOINS the watchdog before releasing anything it could be
	 * reading. Ordering, not tidiness: reversing it is a use-after-free that
	 * only appears under load.
	 */
	luaext_timers_shutdown();

	UNREGISTER_INI_ENTRIES();

	return SUCCESS;
}

static PHP_RINIT_FUNCTION(luaext)
{
#if defined(COMPILE_DL_LUAEXT) && defined(ZTS)
	ZEND_TSRMLS_CACHE_UPDATE();
#endif

	return SUCCESS;
}

static PHP_RSHUTDOWN_FUNCTION(luaext)
{
	/*
	 * A Lua heap is plain malloc, so an unclosed sandbox would survive the
	 * request and leak. Closing them here is also what makes the "no Lua
	 * execution state exists between requests" guarantee true regardless of what
	 * the host forgot to do.
	 */
	luaext_sandbox *sandbox = LUAEXT_G(live_sandboxes);

	while (sandbox != NULL) {
		luaext_sandbox *next = sandbox->live_next;

		luaext_sandbox_close(sandbox);
		sandbox = next;
	}

	/*
	 * The list is deliberately not reset here. luaext_sandbox_close() unlinks
	 * each entry, so an empty list is what correct unlinking produces — and
	 * the GSHUTDOWN assertion exists to catch the case where it does not.
	 * Clearing the head unconditionally would make that assertion unfalsifiable.
	 */
	LUAEXT_ASSERT(LUAEXT_G(live_sandboxes) == NULL);
	LUAEXT_ASSERT(LUAEXT_G(live_count) == 0);

	return SUCCESS;
}

static PHP_MINFO_FUNCTION(luaext)
{
	php_info_print_table_start();
	php_info_print_table_row(2, "luaext support", "enabled");
	php_info_print_table_row(2, "Extension version", PHP_LUAEXT_VERSION);
	php_info_print_table_row(2, "Lua version", LUA_RELEASE);
	php_info_print_table_row(2, "Interpreter", "vendored, statically linked");
#ifdef ZTS
	php_info_print_table_row(2, "Thread safety", "enabled");
#else
	php_info_print_table_row(2, "Thread safety", "disabled");
#endif
#ifdef LUAEXT_DEBUG
	php_info_print_table_row(2, "Debug assertions", "enabled");
#else
	php_info_print_table_row(2, "Debug assertions", "disabled");
#endif
	/*
	 * The same values Sandbox::features() reports, from the same probes, so the
	 * two cannot disagree. This row claimed the limit was unimplemented long
	 * after CPU limits were being enforced and asserted by the suite -- and
	 * phpinfo() is the first place anyone deploying this looks.
	 *
	 * PLATFORM statements, like features(): whether a PARTICULAR limit degrades
	 * because it was set near the clock's resolution is decided when it is set.
	 */
	/* An operator needs to see whether this deployment will load bytecode
	 * nothing can vouch for, without reading php.ini to find out. */
	php_info_print_table_row(2, "Unsealed bytecode",
							 LUAEXT_G(allow_raw_bytecode) ? "allowed" : "refused");

	php_info_print_table_row(2, "CPU limit enforcement",
							 luaext_limit_support_name(luaext_timers_cpu_support()));
	php_info_print_table_row(2, "Wall-clock limit enforcement",
							 luaext_limit_support_name(luaext_timers_wall_support()));

	{
		char resolution[64];

		snprintf(resolution, sizeof(resolution), "%.9g seconds",
				 luaext_timers_cpu_resolution_seconds());
		php_info_print_table_row(2, "CPU clock resolution", resolution);
	}

	php_info_print_table_end();

	DISPLAY_INI_ENTRIES();
}

/* -------------------------------------------------------------------------
 * Module entry
 * ---------------------------------------------------------------------- */

/*
 * The exception hierarchy extends SPL's RuntimeException and LogicException,
 * and SandboxStats implements JsonSerializable, so both modules must have run
 * their own MINIT before ours does.
 */
static const zend_module_dep luaext_deps[] = {ZEND_MOD_REQUIRED("spl") ZEND_MOD_REQUIRED("json")
												  ZEND_MOD_END};

zend_module_entry luaext_module_entry = {
	STANDARD_MODULE_HEADER_EX,
	NULL,
	luaext_deps,
	PHP_LUAEXT_NAME,
	NULL, /* no plain functions: the API is entirely class based */
	PHP_MINIT(luaext),
	PHP_MSHUTDOWN(luaext),
	PHP_RINIT(luaext),
	PHP_RSHUTDOWN(luaext),
	PHP_MINFO(luaext),
	PHP_LUAEXT_VERSION,
	PHP_MODULE_GLOBALS(luaext),
	PHP_GINIT(luaext),
	PHP_GSHUTDOWN(luaext),
	NULL,
	STANDARD_MODULE_PROPERTIES_EX};

#ifdef COMPILE_DL_LUAEXT
#ifdef ZTS
ZEND_TSRMLS_CACHE_DEFINE()
#endif
ZEND_GET_MODULE(luaext)
#endif
