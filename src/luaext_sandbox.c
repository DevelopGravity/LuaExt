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
#include "luaext_defer.h"
#include "luaext_error.h"
#include "luaext_exec.h"
#include "luaext_openlibs.h"
#include "luaext_output.h"
#include "luaext_phpcall.h"
#include "luaext_timers.h"
#include "luaext_profiler.h"
#include "luaext_require.h"
#include "luaext_vfs.h"

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
const char luaext_key_handles = 0;
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
 * Reached only when Lua raises outside a protected call, which is meant to be
 * unreachable: every entry into the interpreter goes through lua_pcall, and the
 * host-side helpers that build Lua values from a PHP method protect their own
 * pushes (luaext_phpcall_push, luaext_require_preload) precisely so that a
 * memory error there unwinds to them instead of arriving here.
 *
 * THE OLD TODO HERE SAID TO RAISE PanicError INSTEAD. That is not implementable,
 * and the reason is worth writing down so nobody spends another afternoon on it:
 * a panic function may not return. Lua calls abort() the moment it does, so
 * throwing a PHP exception and returning 0 would trade a controlled request
 * failure for killing the whole process -- strictly worse in a worker SAPI. The
 * only ways out are longjmp to a recovery point, which needs a setjmp at every
 * unprotected entry and there are none left to put one at, or ending the request.
 *
 * So it ends the request, and the cost is honest rather than hidden:
 * zend_error_noreturn bails out past lua_close, and the Lua heap -- malloc'd
 * rather than emalloc'd, so PHP's allocator will not reclaim it -- is leaked for
 * the life of the process. That is the price of a condition that should never
 * occur; making it cheaper would mean making it survivable, and a state that has
 * panicked is not one to keep running scripts on.
 *
 * The flag is set first so anything that still looks at this sandbox during
 * shutdown sees that its interpreter is not to be touched.
 */
static int luaext_sandbox_panic(lua_State *L)
{
	luaext_sandbox *sandbox = LUAEXT_SB(L);
	const char *message = lua_tostring(L, -1);

	if (sandbox != NULL) {
		sandbox->panicked = true;
	}

	zend_error_noreturn(E_ERROR, "luaext: unprotected error in the Lua interpreter: %s",
						message != NULL ? message : "(no message)");

	return 0;
}

/*
 * String-hash seed. Lua's own luaL_makeseed() derives entropy from heap and
 * function addresses, which a script could read back out; the platform CSPRNG
 * gives the same hash-flooding protection without leaking the layout.
 */
static unsigned int luaext_sandbox_seed(const luaext_policy *policy)
{
	unsigned int seed = 0;

	/*
	 * A host that asked for a specific seed gets it. SandboxConfig refuses a
	 * fixed seed unless deterministic: true was passed alongside it, so
	 * reaching here means surrendering hash-flood protection was a deliberate,
	 * stated decision rather than an accident.
	 */
	if (policy->seed_is_fixed) {
		return (unsigned int)policy->seed;
	}

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

	/*
	 * Before lua_close(). Detaching clears the interrupt flag, and finalisers
	 * run during teardown -- one that saw a pending interrupt would throw out
	 * of the close itself.
	 */
	luaext_timers_detach(sandbox);
	luaext_output_shutdown(sandbox);
	luaext_vfs_shutdown(sandbox);
	luaext_require_shutdown(sandbox);
	luaext_profiler_shutdown(sandbox);

	L = sandbox->L;
	sandbox->L = NULL;
	sandbox->running_L = NULL;

	if (L != NULL) {
		lua_close(L);
	}

	/*
	 * After lua_close(), which is the whole point: closing runs every pending
	 * finaliser, and those hand their PHP references here rather than releasing
	 * them mid-sweep. By this line the state does not exist, so a __destruct
	 * released below cannot re-enter it -- the sandbox is already marked closed,
	 * so one that touches it gets a ClosedSandboxError rather than undefined
	 * behaviour.
	 */
	luaext_defer_shutdown(sandbox);

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
	sandbox->seed = luaext_sandbox_seed(&sandbox->policy);

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
	LUAEXT_ASSERT(luaext_error_is_ready(sandbox));

	/* Before the libraries: os.clock reports billed CPU, and the count hook the
	 * limits ride on has to be installed before any script can exist. */
	if (!luaext_timers_attach(sandbox)) {
		RETURN_THROWS();
	}

	if (!luaext_output_init(sandbox, config)) {
		RETURN_THROWS();
	}

	if (!luaext_vfs_init_from_config(sandbox, config)) {
		RETURN_THROWS();
	}

	if (!luaext_require_init_from_config(sandbox, config)) {
		RETURN_THROWS();
	}

	if (!luaext_openlibs_install(sandbox)) {
		RETURN_THROWS();
	}

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

	/*
	 * A host callback invoked from Lua can reach the sandbox that is running it
	 * -- registerObject() and any closure capturing $sandbox both hand it over.
	 * Closing there would run lua_close() on the very state executing the frame
	 * we would return into, so this is a use-after-free reachable from ordinary
	 * host code rather than a misuse worth documenting.
	 *
	 * Only the method refuses. luaext_sandbox_close() is also the destructor and
	 * RSHUTDOWN sweep's path, and both legitimately run during teardown, when
	 * these depths say nothing useful.
	 */
	if (sandbox->in_lua > 0 || sandbox->in_php > 0) {
		zend_throw_exception(luaext_ce_configuration_error,
							 "Cannot close a sandbox while it is running: close() was called from "
							 "inside a call into Lua, whose state is still executing",
							 0);
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

static void luaext_add_limit_support(zval *array, const char *key, luaext_limit_support support)
{
	static const char *const names[] = {"Enforced", "Degraded", "Unsupported"};
	zval value;

	/* Indexing an array with a value from another translation unit. A support
	 * level this does not recognise is reported as Unsupported: over-reporting
	 * a limit is the one direction this method must never fail in. */
	if ((size_t)support >= sizeof(names) / sizeof(names[0])) {
		support = LUAEXT_LIMIT_UNSUPPORTED;
	}

	ZVAL_OBJ_COPY(&value, zend_enum_get_case_cstr(luaext_ce_limit_support, names[support]));
	add_assoc_zval(array, key, &value);
}

/*
 * Which capabilities this BUILD implements, as opposed to which ones a
 * Capabilities object will accept.
 *
 * The two are not the same thing today and the difference is invisible without
 * this map. A flag whose subsystem has not been written is still a perfectly
 * valid property: it is set, it reads back, and the feature it names is simply
 * absent -- `coroutine` is nil, there is no file API -- so a host granting it
 * has no way to notice short of testing for the behaviour itself. The two flags
 * that CAN be refused at construction are (vfs and vfsWrite, which have no
 * backing store); `coroutines` cannot, because it defaults to true, and
 * refusing it would break every default sandbox.
 *
 * So the honest mechanism is to publish the answer rather than to guess, in the
 * same spirit as reporting a limit's real enforcement level above. A host that
 * must not run a script without coroutines can ask, instead of discovering it
 * from a nil global at runtime.
 *
 * Update this table when a subsystem lands. It is deliberately a literal list
 * rather than something derived from the capability bits: the bits record what
 * may be requested, and nothing in them knows whether anyone implemented it.
 */
static void luaext_add_capability_support(zval *array)
{
	static const struct {
		const char *name;
		bool implemented;
	} capabilities[] = {
		{"loadBytecode", true},
		{"compileAtRuntime", true},
		{"dumpBytecode", true},
		{"require", true},
		{"vfs", true},
		{"vfsWrite", true},
		{"coroutines", true},
		{"osTime", true},
		{"osEnv", true},
		{"debugTraceback", true},
		{"debugIntrospect", true},
		{"debugMutate", true},
		{"debugHooks", true},
		{"utf8", true},
		{"gcControl", true},
		{"warn", true},
	};

	zval map;
	size_t index;

	array_init_size(&map, sizeof(capabilities) / sizeof(capabilities[0]));

	for (index = 0; index < sizeof(capabilities) / sizeof(capabilities[0]); index++) {
		add_assoc_bool(&map, capabilities[index].name, capabilities[index].implemented ? 1 : 0);
	}

	add_assoc_zval(array, "capabilities", &map);
}

ZEND_METHOD(DevelopGravity_LuaExt_Sandbox, features)
{
	ZEND_PARSE_PARAMETERS_NONE();

	array_init_size(return_value, 6);

	/*
	 * Answered by the timer layer, which knows what this platform's clocks can
	 * actually do. This method exists so that a host which must not run
	 * untrusted code without a CPU limit can find out that it has none, so the
	 * one thing it may never do is report a limit it does not enforce.
	 *
	 * These are PLATFORM statements. Whether a PARTICULAR limit degrades --
	 * because it was set close to the clock's resolution -- is decided when it
	 * is set, since this method is static and has no sandbox to ask.
	 */
	luaext_add_limit_support(return_value, "cpuLimit", luaext_timers_cpu_support());
	luaext_add_limit_support(return_value, "wallClockLimit", luaext_timers_wall_support());

	add_assoc_double(return_value, "cpuResolutionSeconds", luaext_timers_cpu_resolution_seconds());

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

	luaext_add_capability_support(return_value);
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
	if (!lua_isfunction(luaext_exec_state(sandbox), -1)) {
		lua_State *L = luaext_exec_state(sandbox);
		const char *found = luaL_typename(L, -1);

		lua_pop(L, 1);
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

	converted = luaext_convert_to_zval(sandbox, luaext_exec_state(sandbox), -1, return_value);
	lua_pop(luaext_exec_state(sandbox), 1);

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
	luaext_sandbox *sandbox;
	zval *callback;
	zend_string *name = NULL;

	ZEND_PARSE_PARAMETERS_START(1, 2)
	Z_PARAM_ZVAL(callback)
	Z_PARAM_OPTIONAL
	Z_PARAM_STR_OR_NULL(name)
	ZEND_PARSE_PARAMETERS_END();

	sandbox = Z_LUAEXT_SANDBOX_P(ZEND_THIS);

	if (!luaext_sandbox_check_usable(sandbox)) {
		RETURN_THROWS();
	}

	if (!luaext_phpcall_push(sandbox, callback, name != NULL ? ZSTR_VAL(name) : NULL)) {
		RETURN_THROWS();
	}

	/* Pops the closure luaext_phpcall_push() left on the stack. This pairing is
	 * the one place the callback and execution subsystems touch. */
	luaext_exec_make_function(sandbox, ZEND_THIS, return_value);
}

ZEND_METHOD(DevelopGravity_LuaExt_Sandbox, registerLibrary)
{
	luaext_sandbox *sandbox;
	zend_string *name;
	HashTable *functions;

	ZEND_PARSE_PARAMETERS_START(2, 2)
	Z_PARAM_STR(name)
	Z_PARAM_ARRAY_HT(functions)
	ZEND_PARSE_PARAMETERS_END();

	sandbox = Z_LUAEXT_SANDBOX_P(ZEND_THIS);

	if (!luaext_sandbox_check_usable(sandbox)) {
		RETURN_THROWS();
	}

	if (!luaext_phpcall_register_table(sandbox, ZSTR_VAL(name), ZSTR_LEN(name), functions)) {
		RETURN_THROWS();
	}
}

ZEND_METHOD(DevelopGravity_LuaExt_Sandbox, registerObject)
{
	luaext_sandbox *sandbox;
	zend_string *name;
	zval *instance;
	HashTable *allowlist = NULL;
	HashTable *methods;
	bool registered;

	ZEND_PARSE_PARAMETERS_START(2, 3)
	Z_PARAM_STR(name)
	Z_PARAM_OBJECT(instance)
	Z_PARAM_OPTIONAL
	Z_PARAM_ARRAY_HT_OR_NULL(allowlist)
	ZEND_PARSE_PARAMETERS_END();

	sandbox = Z_LUAEXT_SANDBOX_P(ZEND_THIS);

	if (!luaext_sandbox_check_usable(sandbox)) {
		RETURN_THROWS();
	}

	/* Selection happens before anything is exposed, so a refusal leaves the
	 * script's view of the world untouched. */
	methods = luaext_phpcall_collect_methods(instance, allowlist);

	if (methods == NULL) {
		RETURN_THROWS();
	}

	registered = luaext_phpcall_register_table(sandbox, ZSTR_VAL(name), ZSTR_LEN(name), methods);
	zend_array_destroy(methods);

	if (!registered) {
		RETURN_THROWS();
	}
}

ZEND_METHOD(DevelopGravity_LuaExt_Sandbox, preloadModule)
{
	luaext_sandbox *sandbox;
	zend_string *name;
	zval *loader;
	lua_State *L;

	ZEND_PARSE_PARAMETERS_START(2, 2)
	Z_PARAM_STR(name)
	Z_PARAM_ZVAL(loader)
	ZEND_PARSE_PARAMETERS_END();

	sandbox = Z_LUAEXT_SANDBOX_P(ZEND_THIS);

	if (!luaext_sandbox_check_usable(sandbox)) {
		RETURN_THROWS();
	}

	/*
	 * Refused when the capability is absent rather than stored for later. A
	 * preload that no require() can ever reach is a host mistake, and reporting
	 * it here names the line that made it -- storing it silently would surface
	 * as a module simply not being found, somewhere else entirely.
	 */
	if (!luaext_has_cap(&sandbox->policy, LUAEXT_CAP_REQUIRE)) {
		zend_throw_exception(luaext_ce_capability_error,
							 "Preloading a module needs the require capability, which this sandbox "
							 "was not granted",
							 0);
		RETURN_THROWS();
	}

	L = sandbox->L;

	if (!lua_checkstack(L, 4)) {
		zend_throw_exception(luaext_ce_memory_limit_error,
							 "Cannot preload a module: the interpreter stack cannot grow", 0);
		RETURN_THROWS();
	}

	/*
	 * A LuaFunction is already a Lua value and goes in as itself; anything else
	 * is a PHP callable and becomes a bound closure, the same one registerLibrary
	 * builds. Either way what lands in package.preload is a Lua function, so
	 * require() has one shape to call rather than two.
	 */
	if (Z_TYPE_P(loader) == IS_OBJECT &&
		instanceof_function(Z_OBJCE_P(loader), luaext_ce_lua_function)) {
		luaext_function_obj *handle = luaext_function_from_obj(Z_OBJ_P(loader));

		if (handle->ref < 0) {
			zend_throw_exception(luaext_ce_configuration_error,
								 "That LuaFunction belongs to a sandbox that has been closed", 0);
			RETURN_THROWS();
		}

		luaext_convert_ref_push(sandbox, L, handle->ref);
	} else if (!luaext_phpcall_push(sandbox, loader, ZSTR_VAL(name))) {
		RETURN_THROWS();
	}

	if (!luaext_require_preload(L, sandbox, ZSTR_VAL(name), ZSTR_LEN(name))) {
		RETURN_THROWS();
	}
}

/*
 * Seconds to nanoseconds, with null meaning "no ceiling".
 *
 * Zero would be a second spelling of that, and a limit no script could ever
 * satisfy is far likelier to be a mistake than an intention -- so it is refused
 * rather than quietly reinterpreted, exactly as setMemoryLimit() refuses zero.
 */
/*
 * Seconds to nanoseconds, refusing anything the conversion cannot represent.
 *
 * Shares LUAEXT_LIMIT_MAX_SECONDS with the SandboxConfig path so that the same
 * number means the same thing however it arrives -- see luaext_types.h for why
 * the bound exists at all.
 */
static bool luaext_sandbox_limit_ns(double seconds, bool unlimited, uint64_t *out)
{
	if (unlimited) {
		*out = 0;
		return true;
	}

	/* Written as a positive test so NAN, which compares false against
	 * everything, is refused by the same condition rather than slipping past a
	 * negated one. Zero is refused rather than treated as a second spelling of
	 * "no ceiling", exactly as setMemoryLimit() refuses it: a limit no script
	 * could ever satisfy is far likelier to be a mistake than an intention. */
	if (!(seconds > 0.0)) {
		zend_argument_value_error(1, "must be greater than 0, or null to lift the limit");
		return false;
	}

	/*
	 * Saturate, matching what a Limits object does with the same number -- see
	 * luaext_config.c. A deadline past LUAEXT_LIMIT_MAX_SECONDS cannot be held
	 * in nanoseconds, and casting it would be undefined behaviour whose result
	 * can be zero, which this API reads as "no ceiling". setCpuLimit(INF) would
	 * then produce a sandbox with no CPU limit at all.
	 */
	if (seconds >= LUAEXT_LIMIT_MAX_SECONDS) {
		*out = UINT64_MAX;
		return true;
	}

	*out = (uint64_t)(seconds * 1e9);
	return true;
}

ZEND_METHOD(DevelopGravity_LuaExt_Sandbox, setCpuLimit)
{
	luaext_sandbox *sandbox;
	double seconds = 0.0;
	bool unlimited = true;
	uint64_t ns;

	ZEND_PARSE_PARAMETERS_START(1, 1)
	Z_PARAM_DOUBLE_OR_NULL(seconds, unlimited)
	ZEND_PARSE_PARAMETERS_END();

	sandbox = Z_LUAEXT_SANDBOX_P(ZEND_THIS);

	if (!luaext_sandbox_check_usable(sandbox) ||
		!luaext_sandbox_limit_ns(seconds, unlimited, &ns) ||
		!luaext_timers_set_cpu_limit(sandbox, ns)) {
		RETURN_THROWS();
	}
}

ZEND_METHOD(DevelopGravity_LuaExt_Sandbox, setWallClockLimit)
{
	luaext_sandbox *sandbox;
	double seconds = 0.0;
	bool unlimited = true;
	uint64_t ns;

	ZEND_PARSE_PARAMETERS_START(1, 1)
	Z_PARAM_DOUBLE_OR_NULL(seconds, unlimited)
	ZEND_PARSE_PARAMETERS_END();

	sandbox = Z_LUAEXT_SANDBOX_P(ZEND_THIS);

	if (!luaext_sandbox_check_usable(sandbox) ||
		!luaext_sandbox_limit_ns(seconds, unlimited, &ns) ||
		!luaext_timers_set_wall_limit(sandbox, ns)) {
		RETURN_THROWS();
	}
}

ZEND_METHOD(DevelopGravity_LuaExt_Sandbox, pauseTimers)
{
	luaext_sandbox *sandbox;

	ZEND_PARSE_PARAMETERS_NONE();

	sandbox = Z_LUAEXT_SANDBOX_P(ZEND_THIS);

	if (!luaext_sandbox_check_usable(sandbox)) {
		RETURN_THROWS();
	}

	/* False rather than an error when the pause is refused: a callback nested
	 * under a frame that did not pause is not misusing the API, it simply does
	 * not get to un-bill its caller's time. */
	RETURN_BOOL(luaext_timers_pause(sandbox, LUAEXT_TIMER_CPU | LUAEXT_TIMER_WALL));
}

ZEND_METHOD(DevelopGravity_LuaExt_Sandbox, resumeTimers)
{
	luaext_sandbox *sandbox;

	ZEND_PARSE_PARAMETERS_NONE();

	sandbox = Z_LUAEXT_SANDBOX_P(ZEND_THIS);

	if (!luaext_sandbox_check_usable(sandbox)) {
		RETURN_THROWS();
	}

	luaext_timers_resume(sandbox, LUAEXT_TIMER_CPU | LUAEXT_TIMER_WALL);
}

ZEND_METHOD(DevelopGravity_LuaExt_Sandbox, interrupt)
{
	luaext_sandbox *sandbox;

	ZEND_PARSE_PARAMETERS_NONE();

	sandbox = Z_LUAEXT_SANDBOX_P(ZEND_THIS);

	/*
	 * The one method a foreign thread may call, so it deliberately does NOT
	 * check thread affinity, and may touch nothing the owning thread writes
	 * without synchronisation -- not even `closed`, and not even to report that
	 * the sandbox is already gone. Two atomic stores, fire and forget.
	 *
	 * The caller must hold a reference for the duration. That cannot be made
	 * airtight from here, because PHP's object refcounts are not atomic; a
	 * caller racing the last reference away has already lost. The stub says so.
	 */
	if (sandbox->owner_thread == luaext_current_thread() && !luaext_sandbox_check_open(sandbox)) {
		RETURN_THROWS();
	}

	luaext_timers_request(sandbox, LUAEXT_IRQ_ABORT);
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
	const luaext_sandbox *sandbox;

	ZEND_PARSE_PARAMETERS_NONE();

	sandbox = Z_LUAEXT_SANDBOX_P(ZEND_THIS);

	if (!luaext_sandbox_check_usable(sandbox)) {
		RETURN_THROWS();
	}

	RETURN_DOUBLE(luaext_timers_cpu_seconds(sandbox));
}

ZEND_METHOD(DevelopGravity_LuaExt_Sandbox, getWallClockUsage)
{
	const luaext_sandbox *sandbox;

	ZEND_PARSE_PARAMETERS_NONE();

	sandbox = Z_LUAEXT_SANDBOX_P(ZEND_THIS);

	if (!luaext_sandbox_check_usable(sandbox)) {
		RETURN_THROWS();
	}

	RETURN_DOUBLE(luaext_timers_wall_seconds(sandbox));
}

static void luaext_sandbox_return_output(INTERNAL_FUNCTION_PARAMETERS, bool take)
{
	luaext_sandbox *sandbox;
	zend_string *output;

	ZEND_PARSE_PARAMETERS_NONE();

	sandbox = Z_LUAEXT_SANDBOX_P(ZEND_THIS);

	if (!luaext_sandbox_check_usable(sandbox)) {
		RETURN_THROWS();
	}

	output = luaext_output_get(sandbox, take);

	if (output == NULL) {
		RETURN_THROWS();
	}

	RETURN_STR(output);
}

ZEND_METHOD(DevelopGravity_LuaExt_Sandbox, getOutput)
{
	luaext_sandbox_return_output(INTERNAL_FUNCTION_PARAM_PASSTHRU, false);
}

ZEND_METHOD(DevelopGravity_LuaExt_Sandbox, takeOutput)
{
	luaext_sandbox_return_output(INTERNAL_FUNCTION_PARAM_PASSTHRU, true);
}

ZEND_METHOD(DevelopGravity_LuaExt_Sandbox, getOutputLength)
{
	const luaext_sandbox *sandbox;
	size_t length;

	ZEND_PARSE_PARAMETERS_NONE();

	sandbox = Z_LUAEXT_SANDBOX_P(ZEND_THIS);

	if (!luaext_sandbox_check_usable(sandbox)) {
		RETURN_THROWS();
	}

	length = luaext_output_length(sandbox);

	if (EG(exception) != NULL) {
		RETURN_THROWS();
	}

	RETURN_LONG((zend_long)length);
}

ZEND_METHOD(DevelopGravity_LuaExt_Sandbox, isOutputTruncated)
{
	const luaext_sandbox *sandbox;
	bool truncated;

	ZEND_PARSE_PARAMETERS_NONE();

	sandbox = Z_LUAEXT_SANDBOX_P(ZEND_THIS);

	if (!luaext_sandbox_check_usable(sandbox)) {
		RETURN_THROWS();
	}

	truncated = luaext_output_truncated(sandbox);

	if (EG(exception) != NULL) {
		RETURN_THROWS();
	}

	RETURN_BOOL(truncated);
}

ZEND_METHOD(DevelopGravity_LuaExt_Sandbox, enableProfiler)
{
	luaext_sandbox *sandbox;
	double period = 0.002;

	ZEND_PARSE_PARAMETERS_START(0, 1)
	Z_PARAM_OPTIONAL
	Z_PARAM_DOUBLE(period)
	ZEND_PARSE_PARAMETERS_END();

	sandbox = Z_LUAEXT_SANDBOX_P(ZEND_THIS);

	if (!luaext_sandbox_check_usable(sandbox)) {
		RETURN_THROWS();
	}

	if (!(period > 0.0)) {
		zend_throw_exception(luaext_ce_configuration_error,
							 "A profiler period must be greater than zero", 0);
		RETURN_THROWS();
	}

	RETURN_BOOL(luaext_profiler_enable(sandbox, period));
}

ZEND_METHOD(DevelopGravity_LuaExt_Sandbox, disableProfiler)
{
	luaext_sandbox *sandbox;

	ZEND_PARSE_PARAMETERS_NONE();

	sandbox = Z_LUAEXT_SANDBOX_P(ZEND_THIS);

	if (!luaext_sandbox_check_usable(sandbox)) {
		RETURN_THROWS();
	}

	luaext_profiler_disable(sandbox);
}

ZEND_METHOD(DevelopGravity_LuaExt_Sandbox, getProfile)
{
	luaext_sandbox *sandbox;
	zval *unit = NULL;
	uint8_t which = 1; /* Seconds, matching the stub's default */

	ZEND_PARSE_PARAMETERS_START(0, 1)
	Z_PARAM_OPTIONAL
	Z_PARAM_OBJECT_OF_CLASS_OR_NULL(unit, luaext_ce_profiler_unit)
	ZEND_PARSE_PARAMETERS_END();

	sandbox = Z_LUAEXT_SANDBOX_P(ZEND_THIS);

	if (!luaext_sandbox_check_usable(sandbox)) {
		RETURN_THROWS();
	}

	if (unit != NULL) {
		zval *name = zend_enum_fetch_case_name(Z_OBJ_P(unit));

		if (name != NULL && Z_TYPE_P(name) == IS_STRING) {
			if (zend_string_equals_literal(Z_STR_P(name), "Samples")) {
				which = 0;
			} else if (zend_string_equals_literal(Z_STR_P(name), "Percent")) {
				which = 2;
			}
		}
	}

	luaext_profiler_result(sandbox, which, return_value);
}
