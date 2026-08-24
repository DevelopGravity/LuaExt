/*
 * luaext — the Sandbox object: an isolated lua_State with its own policy,
 * budget and output sink.
 *
 * This file currently covers the lifecycle, the memory budget and execution:
 * creating the interpreter, opening a library subset, reporting and re-ceiling
 * what it may allocate, compiling and running chunks, reading and writing
 * globals, and tearing it down again. The timing limits, the callback bridge
 * and the output sink arrive with their own subsystems; the methods that need
 * them throw until then.
 *
 * The execution methods are thin on purpose. Each one checks thread affinity
 * and open state, translates its arguments, and hands the work to
 * luaext_exec.c, which owns every entry into the interpreter and the stack
 * discipline that goes with it.
 */

#include "luaext_sandbox.h"

#include "luaext_alloc.h"
#include "luaext_config.h"
#include "luaext_convert.h"
#include "luaext_error.h"
#include "luaext_exec.h"

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
 * A sandbox belongs to the thread that built it. The interpreter has no
 * internal locking and the watchdog will key its CPU clock to one thread, so a
 * second thread touching the state is a data race first and a use-after-free
 * once close() can run concurrently with execution.
 *
 * interrupt() is the deliberate exception: it only sets an atomic flag, and
 * aborting a runaway script from outside is the whole point of it. It must
 * NOT call this.
 *
 * owner_thread is zero until construction succeeds, which keeps a
 * part-constructed object usable by the thread that is still building it.
 */
static bool luaext_sandbox_check_thread(const luaext_sandbox *sandbox)
{
	if (sandbox->owner_thread != 0 && sandbox->owner_thread != luaext_current_thread()) {
		zend_throw_exception(luaext_ce_thread_affinity_error,
							 "A sandbox may only be used from the thread that created it; "
							 "only interrupt() may be called from another thread",
							 0);
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
		if (!luaext_sandbox_check_thread(sandbox) || !luaext_sandbox_check_open(sandbox)) {        \
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
	 * A NULL config resolves to the untrusted baseline, so an unconfigured
	 * sandbox is the safe one. Resolution can refuse — an unsatisfiable
	 * combination throws here rather than producing a sandbox whose limits
	 * quietly do not mean what the host asked for.
	 */
	if (!luaext_config_resolve(config, &sandbox->policy)) {
		RETURN_THROWS();
	}

	/* Retained because the filesystem, resolver and output callback it holds
	 * must outlive this call. */
	if (config != NULL) {
		ZVAL_COPY(&sandbox->config_zv, config);
	}

	/*
	 * Set before the interpreter exists, so the state's own allocations already
	 * count against the budget and a limit too small to hold an interpreter
	 * fails construction rather than being discovered later.
	 */
	sandbox->alloc.limit = sandbox->policy.limits.memory_bytes;
	sandbox->owner_thread = luaext_current_thread();
	sandbox->seed = luaext_sandbox_seed();

	sandbox->L = lua_newstate(luaext_lua_alloc, sandbox, (unsigned int)sandbox->seed);

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

	/*
	 * Before any library is opened, because it is what makes an error
	 * unforgeable. Without the metatable this installs, the error subsystem
	 * falls back to raising a plain Lua string — which a script can catch with
	 * pcall. A limit breach that a script can catch is not a limit, so this is
	 * not an optional step and its absence must never be silent.
	 */
	luaext_error_init(sandbox);
	ZEND_ASSERT(luaext_error_is_ready(sandbox));

	luaext_sandbox_open_libraries(sandbox);
	luaext_sandbox_link(sandbox);
}

ZEND_METHOD(DevelopGravity_LuaExt_Sandbox, close)
{
	luaext_sandbox *sandbox;

	ZEND_PARSE_PARAMETERS_NONE();

	sandbox = Z_LUAEXT_SANDBOX_P(ZEND_THIS);

	/*
	 * Closing from a foreign thread would call lua_close() on a state another
	 * thread may be executing, so this is refused rather than raced. The
	 * destructor path deliberately skips the check: an object is only ever
	 * freed by the thread that owns it.
	 */
	if (!luaext_sandbox_check_thread(sandbox)) {
		RETURN_THROWS();
	}

	luaext_sandbox_close(sandbox);
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

	/*
	 * PHP_OS_FAMILY, not PHP_OS. PHP_OS comes from the generated php_config.h,
	 * which does not exist on Windows -- php-src carries a PHP_OS_STR shim for
	 * exactly that reason, but it lives in php_main.h rather than php.h.
	 * PHP_OS_FAMILY is defined by php.h itself on every platform, and is the
	 * normalised name a caller would compare against anyway.
	 */
	add_assoc_string(return_value, "platform", PHP_OS_FAMILY);
}

/* -------------------------------------------------------------------------
 * Memory accounting
 *
 * The counters behind these live in luaext_alloc.c and cover the Lua heap plus
 * whatever the host holds on the script's behalf, so the numbers reported here
 * are the same ones the ceiling is enforced against.
 * ---------------------------------------------------------------------- */

/* Both guards, in the order every method needs them; see luaext_sandbox.h. */
bool luaext_sandbox_check_usable(const luaext_sandbox *sandbox)
{
	return luaext_sandbox_check_thread(sandbox) && luaext_sandbox_check_open(sandbox);
}

ZEND_METHOD(DevelopGravity_LuaExt_Sandbox, getMemoryUsage)
{
	const luaext_sandbox *sandbox;

	ZEND_PARSE_PARAMETERS_NONE();

	sandbox = Z_LUAEXT_SANDBOX_P(ZEND_THIS);

	if (!luaext_sandbox_check_usable(sandbox)) {
		RETURN_THROWS();
	}

	RETURN_LONG((zend_long)luaext_alloc_usage(sandbox));
}

ZEND_METHOD(DevelopGravity_LuaExt_Sandbox, getPeakMemoryUsage)
{
	const luaext_sandbox *sandbox;

	ZEND_PARSE_PARAMETERS_NONE();

	sandbox = Z_LUAEXT_SANDBOX_P(ZEND_THIS);

	if (!luaext_sandbox_check_usable(sandbox)) {
		RETURN_THROWS();
	}

	RETURN_LONG((zend_long)luaext_alloc_peak(sandbox));
}

ZEND_METHOD(DevelopGravity_LuaExt_Sandbox, setMemoryLimit)
{
	luaext_sandbox *sandbox;
	zend_long bytes = 0;
	bool unlimited = true;

	ZEND_PARSE_PARAMETERS_START(1, 1)
	Z_PARAM_LONG_OR_NULL(bytes, unlimited)
	ZEND_PARSE_PARAMETERS_END();

	sandbox = Z_LUAEXT_SANDBOX_P(ZEND_THIS);

	if (!luaext_sandbox_check_usable(sandbox)) {
		RETURN_THROWS();
	}

	/*
	 * Null is the one spelling of "no ceiling". Zero would be a second, and a
	 * limit that no allocation could ever satisfy is far likelier to be a
	 * mistake than an intention, so both it and a negative are refused rather
	 * than quietly reinterpreted.
	 */
	if (!unlimited && bytes <= 0) {
		zend_argument_value_error(1, "must be greater than 0, or null to lift the limit");
		RETURN_THROWS();
	}

	/*
	 * A limit below current usage is accepted and does not unwind anything: the
	 * next allocation that would grow the heap is refused instead. See
	 * luaext_alloc_set_limit().
	 */
	luaext_alloc_set_limit(sandbox, unlimited ? 0 : (size_t)bytes);
}

/* -------------------------------------------------------------------------
 * Execution
 *
 * Everything below delegates to luaext_exec.c once the two guards have passed.
 * The stack bookkeeping, the protected calls and the traceback handler all live
 * there; what belongs here is the argument handling and the capability check
 * that decides whether a chunk may be binary at all.
 * ---------------------------------------------------------------------- */

/*
 * A chunk name is what a traceback and an error message will call this code.
 *
 * The leading marker is Lua's own convention and worth preserving: "=name"
 * prints as-is, "@name" reads as a file path, and anything else is treated as
 * source text and quoted. The defaults the stub declares all use "=", so an
 * error from an anonymous chunk reads `(eval):1:` rather than a quoted copy of
 * the script.
 */
static const char *luaext_sandbox_chunk_name(const zend_string *given, const char *fallback)
{
	return given != NULL ? ZSTR_VAL(given) : fallback;
}

/*
 * Compile without running, so a host can validate a chunk once and reuse it.
 *
 * Text mode always: `code` is a string an untrusted caller may have supplied,
 * and Lua has no bytecode verifier to catch a crafted binary chunk.
 */
ZEND_METHOD(DevelopGravity_LuaExt_Sandbox, compile)
{
	luaext_sandbox *sandbox;
	zend_string *code;
	zend_string *chunk_name = NULL;

	ZEND_PARSE_PARAMETERS_START(1, 2)
	Z_PARAM_STR(code)
	Z_PARAM_OPTIONAL
	Z_PARAM_STR(chunk_name)
	ZEND_PARSE_PARAMETERS_END();

	sandbox = Z_LUAEXT_SANDBOX_P(ZEND_THIS);

	if (!luaext_sandbox_check_usable(sandbox)) {
		RETURN_THROWS();
	}

	if (!luaext_exec_load(sandbox, ZSTR_VAL(code), ZSTR_LEN(code),
						  luaext_sandbox_chunk_name(chunk_name, "=(load)"), false)) {
		RETURN_THROWS();
	}

	luaext_exec_make_function(sandbox, ZEND_THIS, return_value);

	if (EG(exception) != NULL) {
		RETURN_THROWS();
	}
}

/*
 * The one entry point that may hand a binary chunk to the loader.
 *
 * Lua's undumper trusts what it reads: a malformed or hostile blob is arbitrary
 * native execution rather than a parse error, which is why this is a capability
 * and not a flag. A sandbox that was not granted it cannot be talked into
 * loading bytecode by any argument.
 */
ZEND_METHOD(DevelopGravity_LuaExt_Sandbox, compileBinary)
{
	luaext_sandbox *sandbox;
	zend_string *bytecode;
	zend_string *chunk_name = NULL;

	ZEND_PARSE_PARAMETERS_START(1, 2)
	Z_PARAM_STR(bytecode)
	Z_PARAM_OPTIONAL
	Z_PARAM_STR(chunk_name)
	ZEND_PARSE_PARAMETERS_END();

	sandbox = Z_LUAEXT_SANDBOX_P(ZEND_THIS);

	if (!luaext_sandbox_check_usable(sandbox)) {
		RETURN_THROWS();
	}

	if (!luaext_has_cap(&sandbox->policy, LUAEXT_CAP_LOAD_BYTECODE)) {
		zend_throw_exception(luaext_ce_capability_error,
							 "Loading precompiled bytecode requires the loadBytecode capability, "
							 "which this sandbox was not granted",
							 0);
		RETURN_THROWS();
	}

	if (!luaext_exec_load(sandbox, ZSTR_VAL(bytecode), ZSTR_LEN(bytecode),
						  luaext_sandbox_chunk_name(chunk_name, "=(binary)"), true)) {
		RETURN_THROWS();
	}

	luaext_exec_make_function(sandbox, ZEND_THIS, return_value);

	if (EG(exception) != NULL) {
		RETURN_THROWS();
	}
}

ZEND_METHOD(DevelopGravity_LuaExt_Sandbox, eval)
{
	luaext_sandbox *sandbox;
	zend_string *code;
	zend_string *chunk_name = NULL;

	ZEND_PARSE_PARAMETERS_START(1, 2)
	Z_PARAM_STR(code)
	Z_PARAM_OPTIONAL
	Z_PARAM_STR(chunk_name)
	ZEND_PARSE_PARAMETERS_END();

	sandbox = Z_LUAEXT_SANDBOX_P(ZEND_THIS);

	if (!luaext_sandbox_check_usable(sandbox)) {
		RETURN_THROWS();
	}

	if (!luaext_exec_load(sandbox, ZSTR_VAL(code), ZSTR_LEN(code),
						  luaext_sandbox_chunk_name(chunk_name, "=(eval)"), false)) {
		RETURN_THROWS();
	}

	/* Takes the chunk with it, so a failed call leaves nothing behind. */
	if (!luaext_exec_pcall(sandbox, -1, NULL, 0, return_value)) {
		RETURN_THROWS();
	}
}

ZEND_METHOD(DevelopGravity_LuaExt_Sandbox, call)
{
	luaext_sandbox *sandbox;
	zend_string *path;
	zval *args = NULL;
	uint32_t argc = 0;

	ZEND_PARSE_PARAMETERS_START(1, -1)
	Z_PARAM_STR(path)
	Z_PARAM_VARIADIC('*', args, argc)
	ZEND_PARSE_PARAMETERS_END();

	sandbox = Z_LUAEXT_SANDBOX_P(ZEND_THIS);

	if (!luaext_sandbox_check_usable(sandbox)) {
		RETURN_THROWS();
	}

	if (!luaext_exec_push_path(sandbox, ZSTR_VAL(path), ZSTR_LEN(path))) {
		RETURN_THROWS();
	}

	/*
	 * Checked here rather than left to the interpreter so the refusal can name
	 * the path. "attempt to call a nil value" is a true statement about a stack
	 * slot and says nothing about which global the host meant.
	 */
	if (!lua_isfunction(sandbox->L, -1)) {
		const char *found = luaL_typename(sandbox->L, -1);

		lua_pop(sandbox->L, 1);
		zend_throw_exception_ex(luaext_ce_runtime_error, 0,
								"The Lua path \"%s\" names a %s, which is not callable",
								ZSTR_VAL(path), found);
		RETURN_THROWS();
	}

	if (!luaext_exec_pcall(sandbox, -1, args, argc, return_value)) {
		RETURN_THROWS();
	}
}

ZEND_METHOD(DevelopGravity_LuaExt_Sandbox, getGlobal)
{
	luaext_sandbox *sandbox;
	zend_string *path;
	bool converted;

	ZEND_PARSE_PARAMETERS_START(1, 1)
	Z_PARAM_STR(path)
	ZEND_PARSE_PARAMETERS_END();

	sandbox = Z_LUAEXT_SANDBOX_P(ZEND_THIS);

	if (!luaext_sandbox_check_usable(sandbox)) {
		RETURN_THROWS();
	}

	if (!luaext_exec_push_path(sandbox, ZSTR_VAL(path), ZSTR_LEN(path))) {
		RETURN_THROWS();
	}

	converted = luaext_convert_to_zval(sandbox, sandbox->L, -1, return_value);
	lua_pop(sandbox->L, 1);

	if (!converted) {
		RETURN_THROWS();
	}
}

ZEND_METHOD(DevelopGravity_LuaExt_Sandbox, setGlobal)
{
	luaext_sandbox *sandbox;
	zend_string *path;
	zval *value;

	ZEND_PARSE_PARAMETERS_START(2, 2)
	Z_PARAM_STR(path)
	Z_PARAM_ZVAL(value)
	ZEND_PARSE_PARAMETERS_END();

	sandbox = Z_LUAEXT_SANDBOX_P(ZEND_THIS);

	if (!luaext_sandbox_check_usable(sandbox)) {
		RETURN_THROWS();
	}

	/*
	 * Converted before anything is written, so a value with no Lua
	 * representation costs the host an exception rather than a global that was
	 * half assigned or a table that was created for a path nothing ever
	 * reached.
	 */
	if (!luaext_exec_push_value(sandbox, value)) {
		RETURN_THROWS();
	}

	if (!luaext_exec_assign_path(sandbox, ZSTR_VAL(path), ZSTR_LEN(path))) {
		RETURN_THROWS();
	}
}

/* -------------------------------------------------------------------------
 * Pending methods
 *
 * Each of these belongs to a subsystem that has not landed yet. They report a
 * closed sandbox first, so the eventual behaviour of the closed case is already
 * correct, and otherwise refuse plainly.
 * ---------------------------------------------------------------------- */

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
	const luaext_sandbox *sandbox;

	ZEND_PARSE_PARAMETERS_NONE();

	sandbox = Z_LUAEXT_SANDBOX_P(ZEND_THIS);

	if (!luaext_sandbox_check_usable(sandbox)) {
		RETURN_THROWS();
	}

	/*
	 * A snapshot, not a live view: the counters keep moving, and a host holding
	 * one of these expects the numbers it read to stay put.
	 */
	luaext_config_stats_create(sandbox, return_value);
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
