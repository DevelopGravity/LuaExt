/*
 * luaext — the Sandbox object: an isolated lua_State with its own policy,
 * budget and output sink.
 *
 * This file currently covers the lifecycle only: creating the interpreter,
 * opening a library subset, and tearing it down again. Compilation, calls,
 * value conversion, limits and the output sink arrive with their own
 * subsystems; the methods that need them throw until then.
 */

#include "luaext_sandbox.h"

#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>

#include <stdlib.h>

#include <Zend/zend_enum.h>
#include <Zend/zend_exceptions.h>
#include <ext/random/php_random.h>
#include <ext/random/php_random_csprng.h>

#ifdef PHP_WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

/* -------------------------------------------------------------------------
 * Registry keys
 *
 * Declared in luaext_types.h; the address of each byte is the key, so nothing
 * reachable from Lua can name one. The values are never read.
 * ---------------------------------------------------------------------- */

const char luaext_key_refs = 0;
const char luaext_key_errmt = 0;
const char luaext_key_filemt = 0;
const char luaext_key_threads = 0;
const char luaext_key_loaded = 0;
const char luaext_key_preload = 0;
const char luaext_key_loading = 0;
const char luaext_key_zvalmt = 0;

/* -------------------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------------- */

static zend_object_handlers luaext_sandbox_handlers;

/*
 * Identity of the thread a sandbox belongs to. Only interrupt() may be called
 * from anywhere else, and enforcing that needs a comparable thread identity.
 */
static zend_always_inline uintptr_t luaext_current_thread(void)
{
#ifdef PHP_WIN32
	return (uintptr_t)GetCurrentThreadId();
#else
	return (uintptr_t)pthread_self();
#endif
}

/*
 * Reject use of a sandbox whose interpreter is gone. Every method other than
 * close() and isClosed() starts here, so a closed sandbox reports itself
 * rather than dereferencing a freed lua_State.
 */
static bool luaext_sandbox_check_open(const luaext_sandbox *sandbox)
{
	if (sandbox->closed) {
		zend_throw_exception(luaext_ce_closed_sandbox_error, "The sandbox has been closed", 0);
		return false;
	}

	return true;
}

/*
 * Wave 1 ships the interpreter lifecycle only. Methods whose subsystem has not
 * landed yet say so plainly instead of returning a plausible-looking lie.
 *
 * TODO: remove each case as its subsystem lands.
 */
#define LUAEXT_METHOD_PENDING(sandbox, name)                                                       \
	do {                                                                                           \
		if (!luaext_sandbox_check_open(sandbox)) {                                                 \
			RETURN_THROWS();                                                                       \
		}                                                                                          \
		zend_throw_error(NULL, "DevelopGravity\\LuaExt\\Sandbox::%s() is not implemented yet",     \
						 (name));                                                                  \
		RETURN_THROWS();                                                                           \
	} while (0)

/* -------------------------------------------------------------------------
 * Per-thread live list
 *
 * RSHUTDOWN walks this list and closes whatever the host left open, so a
 * request cannot leak a Lua heap even in a worker SAPI.
 * ---------------------------------------------------------------------- */

static void luaext_sandbox_link(luaext_sandbox *sandbox)
{
	sandbox->live_prev = NULL;
	sandbox->live_next = LUAEXT_G(live_sandboxes);

	if (sandbox->live_next != NULL) {
		sandbox->live_next->live_prev = sandbox;
	}

	LUAEXT_G(live_sandboxes) = sandbox;
	LUAEXT_G(live_count)++;
}

static void luaext_sandbox_unlink(luaext_sandbox *sandbox)
{
	if (sandbox->live_prev != NULL) {
		sandbox->live_prev->live_next = sandbox->live_next;
	} else if (LUAEXT_G(live_sandboxes) == sandbox) {
		LUAEXT_G(live_sandboxes) = sandbox->live_next;
	} else {
		/* Construction failed before the sandbox was ever published. */
		return;
	}

	if (sandbox->live_next != NULL) {
		sandbox->live_next->live_prev = sandbox->live_prev;
	}

	sandbox->live_next = NULL;
	sandbox->live_prev = NULL;
	LUAEXT_G(live_count)--;
}

/* -------------------------------------------------------------------------
 * Interpreter
 * ---------------------------------------------------------------------- */

/*
 * Lua's heap is plain malloc rather than the Zend allocator: a sandbox may
 * legally outlive the request that built it in a worker SAPI, and request-local
 * memory would be freed underneath it.
 *
 * TODO: replace with the accounting allocator that enforces Limits::memoryBytes
 * and feeds luaext_alloc_charge()/discharge().
 */
static void *luaext_sandbox_allocate(void *ud, void *ptr, size_t osize, size_t nsize)
{
	(void)ud;
	(void)osize;

	if (nsize == 0) {
		free(ptr);
		return NULL;
	}

	return realloc(ptr, nsize);
}

/*
 * Reached only when Lua raises outside a protected call. Nothing in Wave 1 runs
 * unprotected Lua except luaL_requiref during construction, but an interpreter
 * without a panic function calls abort(), which is never an acceptable outcome
 * for a request.
 *
 * TODO: raise PanicError and mark the sandbox unusable instead of aborting the
 * request outright.
 */
static int luaext_sandbox_panic(lua_State *L)
{
	const char *message = lua_tostring(L, -1);

	zend_error_noreturn(E_ERROR, "luaext: unprotected error in the Lua interpreter: %s",
						message != NULL ? message : "(no message)");

	return 0;
}

/*
 * The libraries a sandbox may be given. io, os and package are absent by
 * construction: liolib.c, loslib.c and loadlib.c are not compiled into the
 * extension at all, so luaopen_io, luaopen_os and luaopen_package do not exist
 * to be called.
 *
 * linit.c is excluded for the same reason, which is why this table exists at
 * all: luaL_openlibs() is a macro over luaL_openselectedlibs(), and that lives
 * in linit.c.
 */
typedef struct {
	uint32_t bit;
	const char *name;
	lua_CFunction open;
} luaext_library;

static const luaext_library luaext_libraries[] = {
	{LUAEXT_LIB_BASE, LUA_GNAME, luaopen_base},
	{LUAEXT_LIB_TABLE, LUA_TABLIBNAME, luaopen_table},
	{LUAEXT_LIB_STR, LUA_STRLIBNAME, luaopen_string},
	{LUAEXT_LIB_MATH, LUA_MATHLIBNAME, luaopen_math},
	{LUAEXT_LIB_UTF8, LUA_UTF8LIBNAME, luaopen_utf8},
	{LUAEXT_LIB_DEBUG, LUA_DBLIBNAME, luaopen_debug},
};

/*
 * Members of the base library that reach outside the sandbox. dofile() and
 * loadfile() open real files through lauxlib's stdio helpers, load() is the
 * compileAtRuntime capability, and warn() reaches stderr through Lua's default
 * warning function.
 *
 * TODO: the library policy replaces these wholesale rather than deleting them
 * afterwards, and gates load()/warn() on their capabilities.
 */
static const char *const luaext_base_removals[] = {
	"dofile",
	"loadfile",
	"load",
	"warn",
};

static void luaext_sandbox_open_libraries(luaext_sandbox *sandbox)
{
	lua_State *L = sandbox->L;
	size_t index;

	for (index = 0; index < sizeof(luaext_libraries) / sizeof(luaext_libraries[0]); index++) {
		const luaext_library *library = &luaext_libraries[index];

		if ((sandbox->policy.open_libs & library->bit) == 0) {
			continue;
		}

		/* Global, so scripts see the usual names; the result is left on the
		 * stack by luaL_requiref and popped here. */
		luaL_requiref(L, library->name, library->open, 1);
		lua_pop(L, 1);
	}

	if ((sandbox->policy.open_libs & LUAEXT_LIB_BASE) != 0) {
		lua_pushglobaltable(L);

		for (index = 0; index < sizeof(luaext_base_removals) / sizeof(luaext_base_removals[0]);
			 index++) {
			lua_pushnil(L);
			lua_setfield(L, -2, luaext_base_removals[index]);
		}

		lua_pop(L, 1);
	}
}

/*
 * String-hash seed. Lua's own luaL_makeseed() derives entropy from heap and
 * function addresses, which a script could read back out; the platform CSPRNG
 * gives the same hash-flooding protection without leaking the layout.
 */
static unsigned int luaext_sandbox_seed(void)
{
	unsigned int seed = 0;

	if (php_random_bytes_silent(&seed, sizeof(seed)) == FAILURE) {
		seed = (unsigned int)php_random_generate_fallback_seed();
	}

	return seed;
}

/*
 * The untrusted baseline, applied to every sandbox until SandboxConfig is
 * plumbed through.
 *
 * TODO: derive this from the SandboxConfig the caller passed.
 */
static void luaext_sandbox_default_policy(luaext_policy *policy)
{
	memset(policy, 0, sizeof(*policy));

	policy->caps = LUAEXT_CAPS_UNTRUSTED;

	/*
	 * The coroutine library is deliberately absent: it is only ever installed
	 * through our own wrapper, which caps live coroutines and stops resume from
	 * swallowing a fatal error. The debug library is absent because untrusted
	 * code gets debug.traceback and nothing else.
	 */
	policy->open_libs =
		LUAEXT_LIB_BASE | LUAEXT_LIB_TABLE | LUAEXT_LIB_STR | LUAEXT_LIB_MATH | LUAEXT_LIB_UTF8;

	policy->limits.memory_bytes = 32 * 1024 * 1024;
	policy->limits.cpu_ns = 1000000000ull;
	policy->limits.wall_ns = 5000000000ull;
	policy->limits.output_bytes = 1024 * 1024;
	policy->limits.output_overflow = LUAEXT_OVERFLOW_FAIL;
	policy->limits.max_live_coroutines = 64;
	policy->limits.max_coroutine_depth = 16;
	policy->limits.max_call_depth = 200;
	policy->limits.max_modules = 64;
	policy->limits.max_require_depth = 16;
	policy->limits.max_string_length = 64 * 1024 * 1024;
	policy->limits.max_source_bytes = 1024 * 1024;
	policy->limits.max_conversion_depth = 64;

	policy->vfs_quota.max_open_handles = 16;
	policy->vfs_quota.max_file_bytes = 1024 * 1024;
	policy->vfs_quota.max_total_bytes = 8 * 1024 * 1024;
	policy->vfs_quota.max_files = 128;
	policy->vfs_quota.max_operations = 10000;
	policy->vfs_quota.max_path_length = 255;
	policy->vfs_quota.max_path_depth = 16;
	policy->vfs_quota.bill_wall_time = false;
}

void luaext_sandbox_close(luaext_sandbox *sandbox)
{
	lua_State *L;

	if (sandbox->closed) {
		return;
	}

	sandbox->closed = true;

	L = sandbox->L;
	sandbox->L = NULL;
	sandbox->running_L = NULL;

	if (L != NULL) {
		lua_close(L);
	}

	luaext_sandbox_unlink(sandbox);

	zval_ptr_dtor(&sandbox->config_zv);
	ZVAL_UNDEF(&sandbox->config_zv);
}

/* -------------------------------------------------------------------------
 * Object handlers
 * ---------------------------------------------------------------------- */

static zend_object *luaext_sandbox_create_object(zend_class_entry *ce)
{
	luaext_sandbox *sandbox = zend_object_alloc(sizeof(luaext_sandbox), ce);

	zend_object_std_init(&sandbox->std, ce);
	object_properties_init(&sandbox->std, ce);
	sandbox->std.handlers = &luaext_sandbox_handlers;

	return &sandbox->std;
}

static void luaext_sandbox_free_object(zend_object *object)
{
	luaext_sandbox_close(luaext_sandbox_from_obj(object));
	zend_object_std_dtor(object);
}

void luaext_sandbox_startup(void)
{
	memcpy(&luaext_sandbox_handlers, &std_object_handlers, sizeof(zend_object_handlers));

	luaext_sandbox_handlers.offset = XtOffsetOf(struct luaext_sandbox, std);
	luaext_sandbox_handlers.free_obj = luaext_sandbox_free_object;

	/*
	 * A sandbox owns an interpreter and a thread affinity; a copy of one could
	 * only ever be a second handle onto the same state, so cloning is refused
	 * rather than silently aliased.
	 */
	luaext_sandbox_handlers.clone_obj = NULL;

	luaext_ce_sandbox->create_object = luaext_sandbox_create_object;
}

/* -------------------------------------------------------------------------
 * Lifecycle methods
 * ---------------------------------------------------------------------- */

ZEND_METHOD(DevelopGravity_LuaExt_Sandbox, __construct)
{
	zval *config = NULL;
	luaext_sandbox *sandbox;

	ZEND_PARSE_PARAMETERS_START(0, 1)
	Z_PARAM_OPTIONAL
	Z_PARAM_OBJECT_OF_CLASS_OR_NULL(config, luaext_ce_sandbox_config)
	ZEND_PARSE_PARAMETERS_END();

	sandbox = Z_LUAEXT_SANDBOX_P(ZEND_THIS);

	/*
	 * Also rejects a closed sandbox: reconstructing one would create a second
	 * interpreter while the closed flag still short-circuits close(), so the
	 * new state could never be torn down and the object would be freed while
	 * still linked into the per-thread live list.
	 */
	if (sandbox->L != NULL || sandbox->closed) {
		zend_throw_exception(luaext_ce_configuration_error,
							 "The sandbox has already been constructed", 0);
		RETURN_THROWS();
	}

	/*
	 * TODO: read capabilities, limits, the filesystem, the module resolver and
	 * the output settings off the config instead of ignoring it. The zval is
	 * already retained because those objects must outlive the call.
	 */
	luaext_sandbox_default_policy(&sandbox->policy);

	if (config != NULL) {
		ZVAL_COPY(&sandbox->config_zv, config);
	}

	sandbox->alloc.limit = sandbox->policy.limits.memory_bytes;
	sandbox->owner_thread = luaext_current_thread();
	sandbox->seed = luaext_sandbox_seed();

	sandbox->L = lua_newstate(luaext_sandbox_allocate, sandbox, (unsigned int)sandbox->seed);

	if (sandbox->L == NULL) {
		zval_ptr_dtor(&sandbox->config_zv);
		ZVAL_UNDEF(&sandbox->config_zv);

		zend_throw_exception(luaext_ce_memory_limit_error, "Could not allocate a Lua interpreter",
							 0);
		RETURN_THROWS();
	}

	/*
	 * Extra space is not zeroed by Lua, so this has to happen before anything
	 * else touches the state: LUAEXT_SB() and the patched interpreter's
	 * interrupt checks both read this slot, and coroutines inherit its value.
	 */
	*(luaext_sandbox **)lua_getextraspace(sandbox->L) = sandbox;

	sandbox->running_L = sandbox->L;
	lua_atpanic(sandbox->L, luaext_sandbox_panic);

	luaext_sandbox_open_libraries(sandbox);
	luaext_sandbox_link(sandbox);
}

ZEND_METHOD(DevelopGravity_LuaExt_Sandbox, close)
{
	ZEND_PARSE_PARAMETERS_NONE();

	luaext_sandbox_close(Z_LUAEXT_SANDBOX_P(ZEND_THIS));
}

ZEND_METHOD(DevelopGravity_LuaExt_Sandbox, isClosed)
{
	ZEND_PARSE_PARAMETERS_NONE();

	RETURN_BOOL(Z_LUAEXT_SANDBOX_P(ZEND_THIS)->closed);
}

/* -------------------------------------------------------------------------
 * Static introspection
 * ---------------------------------------------------------------------- */

ZEND_METHOD(DevelopGravity_LuaExt_Sandbox, extensionVersion)
{
	ZEND_PARSE_PARAMETERS_NONE();

	RETURN_STRING(PHP_LUAEXT_VERSION);
}

ZEND_METHOD(DevelopGravity_LuaExt_Sandbox, luaVersion)
{
	ZEND_PARSE_PARAMETERS_NONE();

	RETURN_STRING(LUA_RELEASE);
}

static void luaext_add_limit_support(zval *array, const char *key, const char *case_name)
{
	zval support;

	ZVAL_OBJ_COPY(&support, zend_enum_get_case_cstr(luaext_ce_limit_support, case_name));
	add_assoc_zval(array, key, &support);
}

ZEND_METHOD(DevelopGravity_LuaExt_Sandbox, features)
{
	ZEND_PARSE_PARAMETERS_NONE();

	array_init_size(return_value, 5);

	/*
	 * Honest by design: the watchdog and the per-thread CPU clocks are a
	 * separate subsystem, and until they exist neither limit is enforced. This
	 * is exactly the answer this method is for -- a host that must not run
	 * untrusted code without a CPU limit can see that it has none.
	 *
	 * TODO: report the real support level and clock resolution.
	 */
	luaext_add_limit_support(return_value, "cpuLimit", "Unsupported");
	luaext_add_limit_support(return_value, "wallClockLimit", "Unsupported");

	add_assoc_double(return_value, "cpuResolutionSeconds", 0.0);

#ifdef ZTS
	add_assoc_bool(return_value, "threadSafe", 1);
#else
	add_assoc_bool(return_value, "threadSafe", 0);
#endif

	add_assoc_string(return_value, "platform", PHP_OS);
}

/* -------------------------------------------------------------------------
 * Pending methods
 *
 * Each of these belongs to a subsystem that has not landed yet. They report a
 * closed sandbox first, so the eventual behaviour of the closed case is already
 * correct, and otherwise refuse plainly.
 * ---------------------------------------------------------------------- */

ZEND_METHOD(DevelopGravity_LuaExt_Sandbox, compile)
{
	LUAEXT_METHOD_PENDING(Z_LUAEXT_SANDBOX_P(ZEND_THIS), "compile");
}

ZEND_METHOD(DevelopGravity_LuaExt_Sandbox, compileBinary)
{
	LUAEXT_METHOD_PENDING(Z_LUAEXT_SANDBOX_P(ZEND_THIS), "compileBinary");
}

ZEND_METHOD(DevelopGravity_LuaExt_Sandbox, eval)
{
	LUAEXT_METHOD_PENDING(Z_LUAEXT_SANDBOX_P(ZEND_THIS), "eval");
}

ZEND_METHOD(DevelopGravity_LuaExt_Sandbox, call)
{
	LUAEXT_METHOD_PENDING(Z_LUAEXT_SANDBOX_P(ZEND_THIS), "call");
}

ZEND_METHOD(DevelopGravity_LuaExt_Sandbox, getGlobal)
{
	LUAEXT_METHOD_PENDING(Z_LUAEXT_SANDBOX_P(ZEND_THIS), "getGlobal");
}

ZEND_METHOD(DevelopGravity_LuaExt_Sandbox, setGlobal)
{
	LUAEXT_METHOD_PENDING(Z_LUAEXT_SANDBOX_P(ZEND_THIS), "setGlobal");
}

ZEND_METHOD(DevelopGravity_LuaExt_Sandbox, wrapCallable)
{
	LUAEXT_METHOD_PENDING(Z_LUAEXT_SANDBOX_P(ZEND_THIS), "wrapCallable");
}

ZEND_METHOD(DevelopGravity_LuaExt_Sandbox, registerLibrary)
{
	LUAEXT_METHOD_PENDING(Z_LUAEXT_SANDBOX_P(ZEND_THIS), "registerLibrary");
}

ZEND_METHOD(DevelopGravity_LuaExt_Sandbox, registerObject)
{
	LUAEXT_METHOD_PENDING(Z_LUAEXT_SANDBOX_P(ZEND_THIS), "registerObject");
}

ZEND_METHOD(DevelopGravity_LuaExt_Sandbox, preloadModule)
{
	LUAEXT_METHOD_PENDING(Z_LUAEXT_SANDBOX_P(ZEND_THIS), "preloadModule");
}

ZEND_METHOD(DevelopGravity_LuaExt_Sandbox, setMemoryLimit)
{
	LUAEXT_METHOD_PENDING(Z_LUAEXT_SANDBOX_P(ZEND_THIS), "setMemoryLimit");
}

ZEND_METHOD(DevelopGravity_LuaExt_Sandbox, setCpuLimit)
{
	LUAEXT_METHOD_PENDING(Z_LUAEXT_SANDBOX_P(ZEND_THIS), "setCpuLimit");
}

ZEND_METHOD(DevelopGravity_LuaExt_Sandbox, setWallClockLimit)
{
	LUAEXT_METHOD_PENDING(Z_LUAEXT_SANDBOX_P(ZEND_THIS), "setWallClockLimit");
}

ZEND_METHOD(DevelopGravity_LuaExt_Sandbox, pauseTimers)
{
	LUAEXT_METHOD_PENDING(Z_LUAEXT_SANDBOX_P(ZEND_THIS), "pauseTimers");
}

ZEND_METHOD(DevelopGravity_LuaExt_Sandbox, resumeTimers)
{
	LUAEXT_METHOD_PENDING(Z_LUAEXT_SANDBOX_P(ZEND_THIS), "resumeTimers");
}

ZEND_METHOD(DevelopGravity_LuaExt_Sandbox, interrupt)
{
	LUAEXT_METHOD_PENDING(Z_LUAEXT_SANDBOX_P(ZEND_THIS), "interrupt");
}

ZEND_METHOD(DevelopGravity_LuaExt_Sandbox, stats)
{
	LUAEXT_METHOD_PENDING(Z_LUAEXT_SANDBOX_P(ZEND_THIS), "stats");
}

ZEND_METHOD(DevelopGravity_LuaExt_Sandbox, getMemoryUsage)
{
	LUAEXT_METHOD_PENDING(Z_LUAEXT_SANDBOX_P(ZEND_THIS), "getMemoryUsage");
}

ZEND_METHOD(DevelopGravity_LuaExt_Sandbox, getPeakMemoryUsage)
{
	LUAEXT_METHOD_PENDING(Z_LUAEXT_SANDBOX_P(ZEND_THIS), "getPeakMemoryUsage");
}

ZEND_METHOD(DevelopGravity_LuaExt_Sandbox, getCpuUsage)
{
	LUAEXT_METHOD_PENDING(Z_LUAEXT_SANDBOX_P(ZEND_THIS), "getCpuUsage");
}

ZEND_METHOD(DevelopGravity_LuaExt_Sandbox, getWallClockUsage)
{
	LUAEXT_METHOD_PENDING(Z_LUAEXT_SANDBOX_P(ZEND_THIS), "getWallClockUsage");
}

ZEND_METHOD(DevelopGravity_LuaExt_Sandbox, getOutput)
{
	LUAEXT_METHOD_PENDING(Z_LUAEXT_SANDBOX_P(ZEND_THIS), "getOutput");
}

ZEND_METHOD(DevelopGravity_LuaExt_Sandbox, takeOutput)
{
	LUAEXT_METHOD_PENDING(Z_LUAEXT_SANDBOX_P(ZEND_THIS), "takeOutput");
}

ZEND_METHOD(DevelopGravity_LuaExt_Sandbox, getOutputLength)
{
	LUAEXT_METHOD_PENDING(Z_LUAEXT_SANDBOX_P(ZEND_THIS), "getOutputLength");
}

ZEND_METHOD(DevelopGravity_LuaExt_Sandbox, isOutputTruncated)
{
	LUAEXT_METHOD_PENDING(Z_LUAEXT_SANDBOX_P(ZEND_THIS), "isOutputTruncated");
}

ZEND_METHOD(DevelopGravity_LuaExt_Sandbox, enableProfiler)
{
	LUAEXT_METHOD_PENDING(Z_LUAEXT_SANDBOX_P(ZEND_THIS), "enableProfiler");
}

ZEND_METHOD(DevelopGravity_LuaExt_Sandbox, disableProfiler)
{
	LUAEXT_METHOD_PENDING(Z_LUAEXT_SANDBOX_P(ZEND_THIS), "disableProfiler");
}

ZEND_METHOD(DevelopGravity_LuaExt_Sandbox, getProfile)
{
	LUAEXT_METHOD_PENDING(Z_LUAEXT_SANDBOX_P(ZEND_THIS), "getProfile");
}
