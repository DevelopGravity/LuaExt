/*
 * luaext — the Sandbox object: an isolated lua_State with its own policy,
 * budget and output sink.
 *
 * This file owns the lifecycle and the object surface: creating the
 * interpreter, opening a library subset, reporting and re-ceiling what it may
 * allocate, compiling and running chunks, reading and writing globals, exposing
 * host callables, profiling, and tearing it all down again.
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
#include "luaext_proxy.h"
#include "luaext_timers.h"
#include "luaext_profiler.h"
#include "luaext_require.h"
#include "luaext_seal.h"
#include "luaext_thread.h"
#include "luaext_vfs.h"

#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>

#include <stdlib.h>

#include <Zend/zend_enum.h>
#include <Zend/zend_exceptions.h>
#include <ext/random/php_random.h>
#include <ext/random/php_random_csprng.h>

/* -------------------------------------------------------------------------
 * Registry keys
 *
 * Declared in luaext_types.h; the address of each byte is the key, so nothing
 * reachable from Lua can name one. The values are never read.
 * ---------------------------------------------------------------------- */

const char luaext_key_refs = 0;
const char luaext_key_proxymts = 0;
const char luaext_key_errmt = 0;
const char luaext_key_filemt = 0;
const char luaext_key_threads = 0;
const char luaext_key_handles = 0;
const char luaext_key_loaded = 0;
const char luaext_key_preload = 0;
const char luaext_key_loading = 0;
const char luaext_key_zvalmt = 0;
const char luaext_key_chunks = 0;
const char luaext_key_pathmt = 0;

/* -------------------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------------- */

static zend_object_handlers luaext_sandbox_handlers;

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
 *
 * Identity comes from luaext_thread_self() -- the one definition of thread
 * identity, whose byte-copy of pthread_t is spelled out in luaext_thread.c.
 * (TSRM's tsrm_thread_id() is not an option: it lives inside TSRM.h's
 * `#ifdef ZTS` and does not exist in an NTS build, which this extension
 * supports and tests.)
 */
static bool luaext_sandbox_check_thread(const luaext_sandbox *sandbox)
{
	if (sandbox->owner_thread != 0 && sandbox->owner_thread != luaext_thread_self()) {
		zend_throw_exception(luaext_ce_thread_affinity_error,
							 "A sandbox may only be used from the thread that created it; "
							 "only interrupt() may be called from another thread",
							 0);
		return false;
	}

	return true;
}

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
 * Raising a PHP exception from here instead is not implementable, and the
 * reason is worth writing down so nobody spends another afternoon on it: a
 * panic function may not return. Lua calls abort() the moment it does, so
 * throwing a PHP exception and returning 0 would trade a controlled request
 * failure for killing the whole process -- strictly worse in a worker SAPI. The
 * only ways out are longjmp to a recovery point, which needs a setjmp at every
 * unprotected entry and there are none left to put one at, or ending the request.
 *
 * So it ends the request, and the cost is honest rather than hidden:
 * zend_error_noreturn bails out past lua_close, and the Lua heap -- malloc'd
 * rather than emalloc'd, so PHP's allocator will not reclaim it -- is leaked for
 * the life of the process (under luaext.use_zend_mm it is request memory
 * instead, and the request's own shutdown reclaims it wholesale). That is the
 * price of a condition that should never occur; making it cheaper would mean
 * making it survivable, and a state that has panicked is not one to keep
 * running scripts on.
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
	 * Before lua_close(). Detaching deliberately leaves the interrupt RAISED
	 * -- see luaext_timers_detach for the reasoning -- so a script __gc
	 * finaliser cannot hang teardown; the vendored patch 0008's L->errorJmp
	 * check is what keeps its cut-short error on the warning path instead of
	 * aborting the close.
	 */
	luaext_timers_detach(sandbox);
	luaext_output_shutdown(sandbox);
	luaext_vfs_shutdown(sandbox);
	luaext_require_shutdown(sandbox);
	luaext_profiler_shutdown(sandbox);

	L = sandbox->L;
	sandbox->L = NULL;
	sandbox->running_L = NULL;

	/*
	 * A STATE THAT IS STILL EXECUTING IS NOT CLOSED. It is abandoned.
	 *
	 * in_lua counts entries into the interpreter and is decremented by the
	 * bracket that incremented it -- so reaching here with it above zero means
	 * that bracket never ran, which means something longjmp'd straight over it.
	 * PHP does exactly that: a fatal error, PHP's own memory_limit, and
	 * max_execution_time all zend_bailout, and a bailout runs no C cleanup, so
	 * it leaves the Lua VM frames on the C stack and this sandbox's bookkeeping
	 * describing a call that is still notionally in progress.
	 *
	 * lua_close() on such a state walks a CallInfo chain whose C frames are gone
	 * and runs every pending __gc finaliser -- which is untrusted Lua -- against
	 * it. That is undefined behaviour reachable from ordinary host code, and no
	 * amount of it happening to work on a simple case makes it defined.
	 *
	 * So the heap is deliberately leaked, for exactly the reason the panic path
	 * above leaks it: it is malloc'd rather than emalloc'd, so PHP's allocator
	 * will not reclaim it, and the process pays for the life of the request.
	 * (Under luaext.use_zend_mm the blocks are request memory and the arena
	 * reclaims them at shutdown -- the abandonment is the same, only the
	 * bill's lifetime differs.) That is the price of a condition that should
	 * not occur, and it is a much better price than a use-after-free. A debug
	 * build WILL report this as a leak in that case; that report is correct
	 * and must not be "fixed".
	 *
	 * The ordinary paths are unaffected: by the time a destructor or the
	 * RSHUTDOWN sweep runs normally, no call is in progress and in_lua is zero.
	 */
	if (L != NULL && sandbox->in_lua == 0) {
		lua_close(L);
	} else if (L != NULL) {
		/*
		 * The abandoned state's finalisers will never run, so the PHP
		 * references its proxies hold would leak with the heap. The heap's
		 * loss is the price documented above; the objects need not be —
		 * the live-proxy list knows each one, and the leaked heap is
		 * precisely what keeps those payloads valid to read.
		 */
		luaext_proxy_release_abandoned(sandbox);
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

	/* After the drain: registry records are never read by finalisers, and the
	 * proxy GC list they anchor is gone with the state. */
	luaext_proxy_shutdown(sandbox);

	if (sandbox->claimed_globals != NULL) {
		zend_hash_destroy(sandbox->claimed_globals);
		pefree(sandbox->claimed_globals, 1);
		sandbox->claimed_globals = NULL;
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

	/* zend_object_std_init() reads ce->default_object_handlers, installed by
	 * startup below — nothing per-instance to stamp. */
	zend_object_std_init(&sandbox->std, ce);
	object_properties_init(&sandbox->std, ce);

	return &sandbox->std;
}

/*
 * Teardown runs in the destructor phase, not the free phase.
 *
 * Closing runs untrusted Lua finalisers, and those call back into the host --
 * an open file handle's __gc flushes through the FileSystem object. When the
 * cycle collector reclaims a sandbox, the destructor phase is the last point
 * at which every other object in the cycle is still intact; by the free phase
 * their contents are already torn down, and a finaliser reaching one would be
 * touching a corpse. free_obj keeps its close() call as the backstop for the
 * shutdown paths that skip destructors -- close() is idempotent, so the second
 * call is a no-op.
 */
static void luaext_sandbox_dtor_object(zend_object *object)
{
	luaext_sandbox_close(luaext_sandbox_from_obj(object));
	zend_objects_destroy_object(object);
}

static void luaext_sandbox_free_object(zend_object *object)
{
	luaext_sandbox_close(luaext_sandbox_from_obj(object));
	zend_object_std_dtor(object);
}

/*
 * Every zval this struct owns outside the properties table, or the cycle
 * collector cannot see it. The config, the FileSystem, the ModuleResolver,
 * the module paths and the output callback all routinely point back at the
 * host object graph that holds the sandbox -- without this handler such a
 * cycle is uncollectable and pins the whole malloc'd Lua heap until
 * RSHUTDOWN, which is no help to a worker that serves many requests.
 *
 * References held inside the interpreter (wrapped callables, preloads) are
 * deliberately absent: they live in Lua's heap, which the collector cannot
 * traverse; close() is what releases those.
 */
static HashTable *luaext_sandbox_get_gc(zend_object *object, zval **table, int *count)
{
	luaext_sandbox *sandbox = luaext_sandbox_from_obj(object);
	zend_get_gc_buffer *buffer = zend_get_gc_buffer_create();

	zend_get_gc_buffer_add_zval(buffer, &sandbox->config_zv);
	zend_get_gc_buffer_add_zval(buffer, &sandbox->filesystem_zv);
	zend_get_gc_buffer_add_zval(buffer, &sandbox->module_resolver_zv);
	zend_get_gc_buffer_add_zval(buffer, &sandbox->module_paths_zv);
	zend_get_gc_buffer_add_zval(buffer, &sandbox->out.callback);

	/* The resolved fcc holds its own reference to the callback (and to
	 * whatever a Closure captured), so it must be visible to the collector
	 * alongside the zval or a cycle through either reference leaks. Guarded:
	 * add_fcc asserts an INITIALIZED cache on debug builds, and a sandbox
	 * that never resolved one -- or already shut its sink down -- has none. */
	if (ZEND_FCC_INITIALIZED(sandbox->out.fcc)) {
		zend_get_gc_buffer_add_fcc(buffer, &sandbox->out.fcc);
	}

	/*
	 * The one deliberate exception to "references inside the interpreter are
	 * absent": each live proxy holds a PHP object the collector must see, or
	 * a proxy of an object that (transitively) references this sandbox is an
	 * uncollectable cycle. The proxy subsystem keeps the side list precisely
	 * so this handler can report it.
	 */
	luaext_proxy_add_gc(sandbox, buffer);

	zend_get_gc_buffer_use(buffer, table, count);

	return zend_std_get_properties(object);
}

void luaext_sandbox_startup(void)
{
	memcpy(&luaext_sandbox_handlers, &std_object_handlers, sizeof(zend_object_handlers));

	luaext_sandbox_handlers.offset = XtOffsetOf(struct luaext_sandbox, std);
	luaext_sandbox_handlers.dtor_obj = luaext_sandbox_dtor_object;
	luaext_sandbox_handlers.free_obj = luaext_sandbox_free_object;
	luaext_sandbox_handlers.get_gc = luaext_sandbox_get_gc;

	/*
	 * A sandbox owns an interpreter and a thread affinity; a copy of one could
	 * only ever be a second handle onto the same state, so cloning is refused
	 * rather than silently aliased.
	 */
	luaext_sandbox_handlers.clone_obj = NULL;

	luaext_ce_sandbox->create_object = luaext_sandbox_create_object;
	luaext_ce_sandbox->default_object_handlers = &luaext_sandbox_handlers;
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

	/*
	 * Captured now, not read per allocation: the state about to be created
	 * must free every block through the allocator that produced it, whatever
	 * the INI reads later. PHP_INI_SYSTEM makes a later change unlikely, but
	 * "unlikely" is not a memory-safety argument.
	 */
	sandbox->alloc.use_zend_mm = LUAEXT_G(use_zend_mm);

	sandbox->owner_thread = luaext_thread_self();
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

	/*
	 * A refusal, not an assert: without that metatable every error the
	 * subsystem raises falls back to a catchable string, and a limit a script
	 * can catch is not a limit. A sandbox that cannot install it — a memory
	 * limit too small to hold the first table — does not get constructed.
	 */
	if (!luaext_error_is_ready(sandbox)) {
		zend_throw_exception(luaext_ce_memory_limit_error,
							 "This sandbox's memory limit is too small to hold its own error "
							 "machinery",
							 0);
		RETURN_THROWS();
	}

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

	/*
	 * SandboxConfig::$classes, registered exactly as registerClass() with no
	 * parameter overrides — configuration comes from each class's attributes.
	 * Last, and only once the sandbox is fully able to plant class tables:
	 * the config validated the list's shape, but the classes themselves are
	 * resolved here, at Sandbox construction, because a config object may
	 * predate them. The first refusal fails construction whole.
	 */
	if (config != NULL) {
		zval holder;
		zval *classes;

		ZVAL_UNDEF(&holder);
		classes = zend_read_property(luaext_ce_sandbox_config, Z_OBJ_P(config),
									 ZEND_STRL("classes"), true, &holder);

		if (classes != NULL && Z_TYPE_P(classes) == IS_ARRAY) {
			zval *class_name;

			ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(classes), class_name)
			{
				ZVAL_DEREF(class_name);

				/* Config creation refuses this shape, so reaching it means a
				 * path around that check; throw rather than trip
				 * RETURN_THROWS()'s pending-exception contract. */
				if (Z_TYPE_P(class_name) != IS_STRING || Z_STRLEN_P(class_name) == 0) {
					zend_throw_exception(
						luaext_ce_configuration_error,
						"SandboxConfig::$classes must hold non-empty class-name strings", 0);
					zval_ptr_dtor(&holder);
					RETURN_THROWS();
				}

				if (!luaext_proxy_register(sandbox, Z_STR_P(class_name), NULL, NULL, NULL)) {
					zval_ptr_dtor(&holder);
					RETURN_THROWS();
				}
			}
			ZEND_HASH_FOREACH_END();
		}

		zval_ptr_dtor(&holder);
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
	zval value;

	/* Named by luaext_limit_support_name(), which phpinfo() also reads: two
	 * tables for one fact would eventually disagree, and both are public claims
	 * about whether a limit is really enforced. */
	ZVAL_OBJ_COPY(&value, zend_enum_get_case_cstr(luaext_ce_limit_support,
												  luaext_limit_support_name(support)));
	add_assoc_zval(array, key, &value);
}

/*
 * Which capabilities this BUILD implements, as opposed to which ones a
 * Capabilities object will accept.
 *
 * Every entry is true today -- every capability the API accepts is implemented
 * -- but the map stays, because the situation it exists for is not permanent.
 * A flag whose subsystem has not been written is still a perfectly valid
 * property: it is set, it reads back, and the feature it names is simply
 * absent, so a host granting it has no way to notice short of testing for the
 * behaviour itself. The two flags that CAN be refused at construction are vfs
 * and vfsWrite, which have no backing store; `coroutines` cannot, because it
 * defaults to true, and refusing it would break every default sandbox.
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

/* -------------------------------------------------------------------------
 * The registration namespace
 *
 * One rule, one table: a global name any register* call claimed belongs to
 * that registration for the sandbox's lifetime, and a later registration
 * wanting it is refused rather than silently overwriting it. setGlobal() is
 * the deliberate free-form write and never consults this.
 * ---------------------------------------------------------------------- */

bool luaext_sandbox_global_available(luaext_sandbox *sandbox, const char *name, size_t name_len)
{
	if (sandbox->claimed_globals != NULL &&
		zend_hash_str_exists(sandbox->claimed_globals, name, name_len)) {
		zend_throw_exception_ex(
			luaext_ce_configuration_error, 0,
			"The Lua name \"%s\" is already taken by an earlier registration on this sandbox",
			name);
		return false;
	}

	return true;
}

void luaext_sandbox_global_claim(luaext_sandbox *sandbox, const char *name, size_t name_len)
{
	zend_string *key;

	if (sandbox->claimed_globals == NULL) {
		sandbox->claimed_globals = pemalloc(sizeof(HashTable), 1);
		zend_hash_init(sandbox->claimed_globals, 8, NULL, NULL, 1);
	}

	key = zend_string_init(name, name_len, 1);
	zend_hash_add_empty_element(sandbox->claimed_globals, key);
	zend_string_release(key);
}

bool luaext_sandbox_global_release(luaext_sandbox *sandbox, const char *name, size_t name_len)
{
	if (sandbox->claimed_globals == NULL ||
		zend_hash_str_del(sandbox->claimed_globals, name, name_len) == FAILURE) {
		zend_throw_exception_ex(luaext_ce_configuration_error, 0,
								"Nothing is registered under the Lua name \"%s\"", name);
		return false;
	}

	return true;
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
 * The same, but for validate() only, which normalises a bare name to "@name".
 *
 * WHY THIS DIFFERS FROM EVERY OTHER METHOD, deliberately, so nobody "fixes" it
 * back: compile() and eval() are thin wrappers over Lua's loader and preserve
 * its conventions, including the one where an unmarked chunk name means source
 * text and gets quoted as [string "..."]. validate() exists *to report a
 * position* -- a chunk name it cannot report against defeats its only purpose,
 * because the line is recovered by stripping the known display name off the
 * front of Lua's message and a quoted name leaves nothing to strip.
 *
 * So `validate($src, 'rule.lua')` answers with chunkName 'rule.lua' and a real
 * line, where compile() with the same string would say [string "rule.lua"].
 * That divergence is the point, and it costs nothing: validate() is newer than
 * every caller, so no existing behaviour changes.
 *
 * The caller owns the returned zend_string when one is produced.
 */
static const char *luaext_sandbox_validate_chunk_name(const zend_string *given,
													  const char *fallback, zend_string **owned)
{
	const char *name;

	*owned = NULL;

	if (given == NULL) {
		return fallback;
	}

	name = ZSTR_VAL(given);

	/* Already marked, or empty -- an empty name is left exactly as it is rather
	 * than turned into a bare "@", which would name nothing either way. */
	if (ZSTR_LEN(given) == 0 || name[0] == '=' || name[0] == '@') {
		return name;
	}

	*owned = zend_strpprintf(0, "@%s", name);

	return ZSTR_VAL(*owned);
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
/*
 * Parse without running and without throwing, so a host can reject a bad script
 * at save time and show its author the line.
 *
 * No capability check, matching compile(): compileAtRuntime gates Lua's own
 * load(), not host-side compilation. The limits DO apply, which is why this is
 * an instance method -- maxSourceBytes is the caller's, not a global.
 *
 * Only a parse refusal becomes a result. A closed sandbox, a cross-thread call
 * or an interpreter that cannot grow its stack are statements about the HOST,
 * not about the script, and reporting them as "your Lua is invalid" would send
 * whoever reads it to the wrong place entirely.
 */
ZEND_METHOD(DevelopGravity_LuaExt_Sandbox, validate)
{
	luaext_sandbox *sandbox;
	zend_string *code;
	zend_string *chunk_name = NULL;
	zend_string *normalised = NULL;
	const char *resolved_name;
	bool parsed;
	zend_object *error;
	const zval *message;
	const char *reported_name;
	zend_long line;
	zval message_rv;

	ZEND_PARSE_PARAMETERS_START(1, 2)
	Z_PARAM_STR(code)
	Z_PARAM_OPTIONAL
	Z_PARAM_STR(chunk_name)
	ZEND_PARSE_PARAMETERS_END();

	sandbox = Z_LUAEXT_SANDBOX_P(ZEND_THIS);

	if (!luaext_sandbox_check_usable(sandbox)) {
		RETURN_THROWS();
	}

	resolved_name = luaext_sandbox_validate_chunk_name(chunk_name, "=(load)", &normalised);

	parsed = luaext_exec_load(sandbox, ZSTR_VAL(code), ZSTR_LEN(code), resolved_name, false);

	if (normalised != NULL) {
		/* Released here rather than at each exit: the load has copied whatever
		 * it needed out of it, and every path below is done with the name. */
		zend_string_release(normalised);
	}

	if (parsed) {
		/* The compiled chunk is ours and nobody asked for it: leaving it behind
		 * would grow the stack by one on every call. */
		lua_pop(luaext_exec_state(sandbox), 1);

		luaext_config_validation_create(return_value, true, NULL, 0, NULL);
		return;
	}

	error = EG(exception);

	if (error == NULL) {
		/* Unreachable: a failed load always throws. Reported rather than
		 * assumed, because returning "valid" here would be the worst answer. */
		zend_throw_exception(luaext_ce_runtime_error,
							 "The chunk failed to compile without reporting why", 0);
		RETURN_THROWS();
	}

	/*
	 * Two refusals are answers about the SCRIPT, and both are what the caller
	 * asked about: it does not parse, or it is larger than this sandbox accepts.
	 * A host deciding whether to store a submission wants "no" either way.
	 *
	 * Anything else -- a closed sandbox, a cross-thread call, an interpreter
	 * that cannot grow its stack -- is a statement about the HOST, and reporting
	 * it as "your Lua is invalid" would send the author to code that is fine.
	 */
	if (!instanceof_function(error->ce, luaext_ce_syntax_error) &&
		!instanceof_function(error->ce, luaext_ce_source_limit_error)) {
		RETURN_THROWS();
	}

	/*
	 * Taken, not cleared: the exception object carries the message and -- since
	 * the compile path now attaches one -- the line and chunk name, so the
	 * result is assembled from it rather than recomputed.
	 */
	GC_ADDREF(error);
	zend_clear_exception();

	ZVAL_UNDEF(&message_rv);
	message =
		zend_read_property_ex(error->ce, error, ZSTR_KNOWN(ZEND_STR_MESSAGE), true, &message_rv);
	luaext_error_lua_position(error, &reported_name, &line);

	luaext_config_validation_create(
		return_value, false,
		(message != NULL && Z_TYPE_P(message) == IS_STRING) ? Z_STR_P(message) : NULL, line,
		reported_name);

	/* After the create: a hooked $message materialised into the scratch, and
	 * the create's copy is the last read of it. */
	zval_ptr_dtor(&message_rv);
	OBJ_RELEASE(error);
}

ZEND_METHOD(DevelopGravity_LuaExt_Sandbox, compileBinary)
{
	luaext_sandbox *sandbox;
	zend_string *bytecode;
	zend_string *chunk_name = NULL;
	const char *payload = NULL;
	size_t payload_len = 0;

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

	/*
	 * Everything below decides ONE question: can this blob be vouched for?
	 *
	 * Lua's loader checks a binary chunk's header and stops -- not its opcodes,
	 * register indices, constant indices or jump targets -- so a blob that is
	 * merely well-formed at the front reaches the VM intact. Corrupting one byte
	 * of a small chunk at each position: 57% refused, 33% ran anyway, 10% killed
	 * the process. There is no verifier to add, so the only safe answer for a
	 * blob nothing can vouch for is to refuse it.
	 */
	if (luaext_seal_is_sealed(ZSTR_VAL(bytecode), ZSTR_LEN(bytecode))) {
		/*
		 * Verified against the mode THIS SANDBOX is configured for, never the
		 * one the blob announces. Trusting the blob's own byte would let an
		 * authenticated chunk be downgraded to a checksummed one: recompute an
		 * xxh128, which is unkeyed and anybody can do, and the key stops
		 * mattering.
		 */
		if (!luaext_seal_open(ZSTR_VAL(bytecode), ZSTR_LEN(bytecode),
							  (luaext_seal_algo)sandbox->policy.seal_mode,
							  sandbox->policy.bytecode_key, sandbox->policy.bytecode_key_len,
							  &payload, &payload_len)) {
			zend_throw_exception(
				luaext_ce_bytecode_integrity_error,
				"This bytecode does not verify. It was sealed by a sandbox configured "
				"differently -- another SealMode, or another SandboxConfig::$bytecodeKey -- or "
				"it has been altered since. Either way it is not something this sandbox will "
				"execute.",
				0);
			RETURN_THROWS();
		}
	} else if (!LUAEXT_G(allow_raw_bytecode)) {
		zend_throw_exception(
			luaext_ce_bytecode_integrity_error,
			"This blob carries no seal, and loading unsealed bytecode is disabled. Anything "
			"dump() produces is sealed and loads without any INI change; set "
			"luaext.allow_raw_bytecode=1 in php.ini only to accept blobs from elsewhere, "
			"which nothing can vouch for.",
			0);
		RETURN_THROWS();
	} else {
		payload = ZSTR_VAL(bytecode);
		payload_len = ZSTR_LEN(bytecode);
	}

	if (!luaext_exec_load(sandbox, payload, payload_len,
						  luaext_sandbox_chunk_name(chunk_name, "=(binary)"), true)) {
		RETURN_THROWS();
	}

	luaext_exec_make_function(sandbox, ZEND_THIS, return_value);

	if (EG(exception) != NULL) {
		RETURN_THROWS();
	}
}

/* -------------------------------------------------------------------------
 * The eval() compile cache
 *
 * eval() parses its source on every call and throws the chunk away, which costs
 * an order of magnitude on a chunk of any size (10.2x on 3.8 KB; see
 * docs/performance.md). When the host opts in, the compiled main chunk is kept
 * in a registry table and reused.
 *
 * WHAT THIS DOES NOT HELP: a sandbox built per request, evaluated once, and
 * closed. Its cache is empty every time. Only a sandbox that outlives several
 * evaluations of the same source gains anything, which is why the feature is
 * off unless asked for rather than simply switched on for everyone.
 * ---------------------------------------------------------------------- */

/* The cache table, created on first use. Same shape as require's tables. */
static void luaext_sandbox_push_chunk_cache(lua_State *L)
{
	if (lua_rawgetp(L, LUA_REGISTRYINDEX, &luaext_key_chunks) == LUA_TTABLE) {
		return;
	}

	lua_pop(L, 1);
	lua_createtable(L, 0, 8);
	lua_pushvalue(L, -1);
	lua_rawsetp(L, LUA_REGISTRYINDEX, &luaext_key_chunks);
}

/*
 * The cache key: the chunk name, a NUL, then the source.
 *
 * A Lua string used directly as a table key, so lookup is exact and no
 * collision reasoning is needed. The chunk name has to be part of it because it
 * decides what every traceback out of that chunk will say -- two identical
 * sources under different names are genuinely different chunks.
 */
static void luaext_sandbox_push_chunk_key(lua_State *L, const char *chunk_name,
										  const zend_string *code)
{
	luaL_Buffer buffer;

	luaL_buffinit(L, &buffer);
	luaL_addstring(&buffer, chunk_name);
	luaL_addchar(&buffer, '\0');
	luaL_addlstring(&buffer, ZSTR_VAL(code), ZSTR_LEN(code));
	luaL_pushresult(&buffer);
}

/*
 * Build the cache table and the key under a protected call, leaving both on the
 * stack. Creating the table and assembling the key both allocate inside the
 * interpreter, and this runs from a PHP method body where a raise has no
 * handler -- the same trampoline every other host-side push uses.
 *
 * Argument 1 is the chunk name, argument 2 the source, both as lightuserdata.
 */
static int luaext_sandbox_push_cache_and_key(lua_State *L)
{
	const char *chunk_name = (const char *)lua_touserdata(L, 1);
	const zend_string *code = (const zend_string *)lua_touserdata(L, 2);

	lua_settop(L, 0);

	luaext_sandbox_push_chunk_cache(L);
	luaext_sandbox_push_chunk_key(L, chunk_name, code);

	return 2;
}

ZEND_METHOD(DevelopGravity_LuaExt_Sandbox, eval)
{
	luaext_sandbox *sandbox;
	zend_string *code;
	zend_string *chunk_name = NULL;
	const char *resolved_name;
	lua_State *L;
	bool cached = false;

	ZEND_PARSE_PARAMETERS_START(1, 2)
	Z_PARAM_STR(code)
	Z_PARAM_OPTIONAL
	Z_PARAM_STR(chunk_name)
	ZEND_PARSE_PARAMETERS_END();

	sandbox = Z_LUAEXT_SANDBOX_P(ZEND_THIS);

	if (!luaext_sandbox_check_usable(sandbox)) {
		RETURN_THROWS();
	}

	resolved_name = luaext_sandbox_chunk_name(chunk_name, "=(eval)");
	L = luaext_exec_state(sandbox);

	if (sandbox->policy.cache_compiled_chunks) {
		if (!lua_checkstack(L, 4)) {
			zend_throw_exception(luaext_ce_memory_limit_error,
								 "Cannot evaluate a chunk: the interpreter stack cannot grow", 0);
			RETURN_THROWS();
		}

		lua_pushcfunction(L, luaext_sandbox_push_cache_and_key);
		lua_pushlightuserdata(L, (void *)(uintptr_t)resolved_name);
		lua_pushlightuserdata(L, (void *)code);

		if (lua_pcall(L, 2, 2, 0) != LUA_OK) {
			lua_pop(L, 1);
			zend_throw_exception(luaext_ce_memory_limit_error,
								 "Cannot evaluate a chunk: its cache key does not fit in the "
								 "sandbox's memory budget",
								 0);
			RETURN_THROWS();
		}

		lua_pushvalue(L, -1); /* keep the key; a miss needs it to store under */

		if (lua_rawget(L, -3) == LUA_TFUNCTION) {
			/* Hit: [cache][key][chunk] -> leave only the chunk. */
			lua_remove(L, -2);
			lua_remove(L, -2);
			cached = true;
		} else {
			lua_pop(L, 1); /* the nil */
		}
	}

	if (!cached) {
		if (!luaext_exec_load(sandbox, ZSTR_VAL(code), ZSTR_LEN(code), resolved_name, false)) {
			if (sandbox->policy.cache_compiled_chunks) {
				lua_pop(L, 2); /* the cache table and the key */
			}

			RETURN_THROWS();
		}

		if (sandbox->policy.cache_compiled_chunks) {
			uint32_t cap = sandbox->policy.limits.max_cached_chunks;

			/*
			 * Stack here is [cache][key][chunk]. Past the ceiling the entry is
			 * simply not stored: a full cache slows the next call down, it does
			 * not fail it, because "your script stopped working once we had
			 * seen enough other scripts" is not a defensible behaviour.
			 */
			if (cap == 0 || sandbox->cached_chunks < (uint64_t)cap) {
				/* rawset pops VALUE then KEY off the top, so both are copied
				 * above the chunk rather than stored from where they sit. */
				lua_pushvalue(L, -2); /* [cache][key][chunk][key] */
				lua_pushvalue(L, -2); /* [cache][key][chunk][key][chunk] */
				lua_rawset(L, -5);	  /* cache[key] = chunk -> [cache][key][chunk] */
				sandbox->cached_chunks++;
				lua_remove(L, -2); /* -> [cache][chunk] */
				lua_remove(L, -2); /* -> [chunk] */
			} else {
				lua_remove(L, -2); /* drop the key  -> [cache][chunk] */
				lua_remove(L, -2); /* drop the cache -> [chunk] */
			}
		}
	}

	/*
	 * Takes the chunk with it, so a failed call leaves nothing behind. A cached
	 * chunk is a value the registry also holds, so consuming the stack copy
	 * costs the cache nothing.
	 */
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
	zend_fcall_info callback_info;
	zend_fcall_info_cache callback_cache;
	zend_string *name = NULL;

	/*
	 * Z_PARAM_FUNC rather than a raw zval: the stub declares `callable`, and
	 * a non-callable argument must raise the engine's own TypeError naming
	 * the argument position -- not a ConfigurationError from the later
	 * resolution, which a `catch (TypeError)` written against the published
	 * stub would never see. The cache is not touched again: Z_PARAM_FUNC
	 * frees a trampoline itself (that is what the _NO_TRAMPOLINE_FREE
	 * variant exists to opt out of), and the push below re-resolves from the
	 * original zval, whose validity ZPP just proved.
	 */
	ZEND_PARSE_PARAMETERS_START(1, 2)
	Z_PARAM_FUNC(callback_info, callback_cache)
	Z_PARAM_OPTIONAL
	Z_PARAM_STR_OR_NULL(name)
	ZEND_PARSE_PARAMETERS_END();

	sandbox = Z_LUAEXT_SANDBOX_P(ZEND_THIS);

	if (!luaext_sandbox_check_usable(sandbox)) {
		RETURN_THROWS();
	}

	if (!luaext_phpcall_push(sandbox, luaext_exec_state(sandbox), &callback_info.function_name,
							 name != NULL ? ZSTR_VAL(name) : NULL)) {
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

	if (!luaext_sandbox_check_usable(sandbox) ||
		!luaext_sandbox_global_available(sandbox, ZSTR_VAL(name), ZSTR_LEN(name))) {
		RETURN_THROWS();
	}

	if (!luaext_phpcall_register_table(sandbox, ZSTR_VAL(name), ZSTR_LEN(name), functions)) {
		RETURN_THROWS();
	}

	luaext_sandbox_global_claim(sandbox, ZSTR_VAL(name), ZSTR_LEN(name));
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

	if (!luaext_sandbox_check_usable(sandbox) ||
		!luaext_sandbox_global_available(sandbox, ZSTR_VAL(name), ZSTR_LEN(name))) {
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

	luaext_sandbox_global_claim(sandbox, ZSTR_VAL(name), ZSTR_LEN(name));
}

ZEND_METHOD(DevelopGravity_LuaExt_Sandbox, registerClass)
{
	luaext_sandbox *sandbox;
	zend_string *class_name;
	HashTable *methods = NULL;
	zend_string *lua_name = NULL;
	HashTable *operators = NULL;

	ZEND_PARSE_PARAMETERS_START(1, 4)
	Z_PARAM_STR(class_name)
	Z_PARAM_OPTIONAL
	Z_PARAM_ARRAY_HT_OR_NULL(methods)
	Z_PARAM_STR_OR_NULL(lua_name)
	Z_PARAM_ARRAY_HT_OR_NULL(operators)
	ZEND_PARSE_PARAMETERS_END();

	sandbox = Z_LUAEXT_SANDBOX_P(ZEND_THIS);

	if (!luaext_sandbox_check_usable(sandbox)) {
		RETURN_THROWS();
	}

	if (!luaext_proxy_register(sandbox, class_name, methods, lua_name, operators)) {
		RETURN_THROWS();
	}
}

/* The allocating half of clearing a global, run under lua_pcall. */
static int luaext_sandbox_clear_global(lua_State *L)
{
	const char *name = (const char *)lua_touserdata(L, 1);

	lua_settop(L, 0);

	/* Raw, like every host-side write into the globals table: a script-
	 * installed _G metamethod must not run unmetered inside a host call. */
	lua_pushglobaltable(L);
	lua_pushstring(L, name);
	lua_pushnil(L);
	lua_rawset(L, 1);

	return 0;
}

ZEND_METHOD(DevelopGravity_LuaExt_Sandbox, unregister)
{
	luaext_sandbox *sandbox;
	zend_string *name;
	lua_State *L;
	int status;

	ZEND_PARSE_PARAMETERS_START(1, 1)
	Z_PARAM_STR(name)
	ZEND_PARSE_PARAMETERS_END();

	sandbox = Z_LUAEXT_SANDBOX_P(ZEND_THIS);

	if (!luaext_sandbox_check_usable(sandbox)) {
		RETURN_THROWS();
	}

	/*
	 * Look, don't commit: releasing the claim (or retiring a class) before
	 * the global is actually cleared would, on a failed clear, leave the
	 * name re-registerable while the stale global still resolves. Nothing
	 * changes until the interpreter half has succeeded.
	 */
	if (sandbox->claimed_globals == NULL ||
		!zend_hash_str_exists(sandbox->claimed_globals, ZSTR_VAL(name), ZSTR_LEN(name))) {
		zend_throw_exception_ex(luaext_ce_configuration_error, 0,
								"Nothing is registered under the Lua name \"%s\"", ZSTR_VAL(name));
		RETURN_THROWS();
	}

	L = sandbox->running_L != NULL ? sandbox->running_L : sandbox->L;

	/* The running state can be a coroutine with no ambient slack; refuse
	 * like the registrars rather than trust it. */
	if (!lua_checkstack(L, 4)) {
		zend_throw_exception(luaext_ce_memory_limit_error,
							 "Cannot unregister a name: the interpreter stack cannot grow", 0);
		RETURN_THROWS();
	}

	lua_pushcfunction(L, luaext_sandbox_clear_global);
	lua_pushlightuserdata(L, (void *)ZSTR_VAL(name));
	status = lua_pcall(L, 1, 0, 0);

	if (status != LUA_OK) {
		luaext_error_throw_from_lua(sandbox, L, status);
		RETURN_THROWS();
	}

	/* Committed only now, and release cannot fail: existence held above. */
	(void)luaext_sandbox_global_release(sandbox, ZSTR_VAL(name), ZSTR_LEN(name));

	/* A class registered under this name stops wrapping new instances;
	 * proxies a script already holds keep working. No-op otherwise. */
	luaext_proxy_retire_name(sandbox, L, name);
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

	/*
	 * No single ZPP macro spells the LuaFunction|callable union, so the raw
	 * zval is taken and the declared type is enforced here, before anything
	 * else -- the same engine TypeError, naming the argument position, that
	 * wrapCallable's Z_PARAM_FUNC delivers for its plain `callable`. Without
	 * this the arginfo type is decoration: internal functions get no VM check,
	 * and a wrong-typed loader would surface as a ConfigurationError from the
	 * later resolution, which a `catch (TypeError)` written against the
	 * published stub would never see.
	 */
	if (!(Z_TYPE_P(loader) == IS_OBJECT &&
		  instanceof_function(Z_OBJCE_P(loader), luaext_ce_lua_function)) &&
		!zend_is_callable(loader, 0, NULL)) {
		zend_argument_type_error(2,
								 "must be of type DevelopGravity\\LuaExt\\LuaFunction|callable, "
								 "%s given",
								 zend_zval_value_name(loader));
		RETURN_THROWS();
	}

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

	/* One state for the push and for luaext_require_preload() below, and the
	 * running one at that -- see luaext_phpcall_push() on why the two can
	 * differ and what reading the wrong one costs. */
	L = luaext_exec_state(sandbox);

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

		/*
		 * A ref names a slot in ONE sandbox's registry table, so a handle from
		 * another sandbox does not fail here -- it silently reads THIS sandbox's
		 * slot of the same number, which is an unrelated function or nil. The
		 * conversion path refuses a foreign handle for exactly that reason
		 * (luaext_convert.c, the LUA_TFUNCTION case); this is the other door into
		 * the same registry and needs the same guard rather than trusting a host
		 * not to mix handles from two sandboxes.
		 *
		 * Ownership before liveness, so a handle from a different sandbox that is
		 * also closed is reported as what it actually is.
		 */
		if (Z_TYPE(handle->sandbox_zv) != IS_OBJECT || Z_OBJ(handle->sandbox_zv) != &sandbox->std) {
			zend_throw_exception(luaext_ce_configuration_error,
								 "That LuaFunction belongs to a different sandbox", 0);
			RETURN_THROWS();
		}

		if (handle->ref < 0) {
			zend_throw_exception(luaext_ce_configuration_error,
								 "That LuaFunction belongs to a sandbox that has been closed", 0);
			RETURN_THROWS();
		}

		luaext_convert_ref_push(sandbox, L, handle->ref);

		/* The slot may have been released since the handle was made. package.preload
		 * must hold a function, or require() would try to call a nil. */
		if (!lua_isfunction(L, -1)) {
			lua_pop(L, 1);
			zend_throw_exception(luaext_ce_configuration_error,
								 "That LuaFunction no longer references a Lua function", 0);
			RETURN_THROWS();
		}
	} else if (!luaext_phpcall_push(sandbox, L, loader, ZSTR_VAL(name))) {
		RETURN_THROWS();
	}

	if (!luaext_require_preload(L, sandbox, ZSTR_VAL(name), ZSTR_LEN(name))) {
		RETURN_THROWS();
	}
}

/* -------------------------------------------------------------------------
 * Limits
 *
 * ONE SETTER, TAKING THE OBJECT THE CONSTRUCTOR TAKES. There used to be three --
 * setMemoryLimit, setCpuLimit, setWallClockLimit -- carried over from the
 * extension this replaces, and between them they could change three of the
 * fourteen limits a SandboxConfig can express. The other eleven were settable
 * only at construction, for no reason anyone could state: every one of them is
 * read from sandbox->policy at the point it applies, so all fourteen were always
 * changeable and only three were reachable.
 *
 * Collapsing them also ended a real inconsistency. A Limits object reads 0.0 as
 * "no ceiling", the same as null; setCpuLimit(0.0) REFUSED it. The same number
 * meant two different things depending on which door it came through. Now there
 * is one door, and luaext_config_limits_read() is the same function the
 * constructor uses, so the rules cannot drift apart again.
 * ---------------------------------------------------------------------- */

ZEND_METHOD(DevelopGravity_LuaExt_Sandbox, setLimits)
{
	luaext_sandbox *sandbox;
	zval *limits_zv;
	luaext_limits limits;

	ZEND_PARSE_PARAMETERS_START(1, 1)
	Z_PARAM_OBJECT_OF_CLASS(limits_zv, luaext_ce_limits)
	ZEND_PARSE_PARAMETERS_END();

	sandbox = Z_LUAEXT_SANDBOX_P(ZEND_THIS);

	if (!luaext_sandbox_check_usable(sandbox)) {
		RETURN_THROWS();
	}

	/* Read into a local first. Nothing is applied until every field has been
	 * accepted, so a Limits the conversion refuses leaves the sandbox on the
	 * limits it already had rather than half-way onto new ones. */
	if (!luaext_config_limits_read(Z_OBJ_P(limits_zv), &limits)) {
		RETURN_THROWS();
	}

	/*
	 * The three that own state elsewhere go through their own subsystems: the
	 * allocator holds the memory ceiling, the timers re-arm a deadline that may
	 * already be counting, and the output sink snapshotted its budget when the
	 * sandbox was built. The remaining eleven are read live off the policy, so
	 * storing the struct is all they need.
	 *
	 * Timers first, because arming is the only step that can still fail -- an
	 * unstartable watchdog refuses a CPU deadline -- and a refusal that lands
	 * after the policy was replaced would leave the sandbox describing limits it
	 * is not enforcing.
	 *
	 * WHAT THAT DOES AND DOES NOT BUY. The policy is all-or-nothing, but the two
	 * timer calls are not atomic with each other, so one can apply and the next
	 * refuse. Working through when: the wall setter refuses only for a zero slot
	 * or an unenforceable sandbox, and the CPU setter demands both of those too
	 * whenever its own limit is non-zero -- so the single surviving case is
	 * cpuSeconds 0 on a sandbox that cannot enforce, where the slot's CPU
	 * deadline is cleared while limits() still names the old one. That is a
	 * sandbox already reporting it can enforce no timing limit at all, so the
	 * divergence is between two descriptions of nothing. Left alone deliberately:
	 * a pre-flight check would duplicate the setters' own refusal rules, and two
	 * copies of that logic drifting apart is the more likely defect.
	 */
	/*
	 * The one rule the setters do NOT carry, because it is about a capability
	 * rather than about a limit: a sandbox granted debugHooks cannot also be
	 * bounded in time, since the script can displace the hook both limits are
	 * delivered through. Construction refuses that pair, and this is the other
	 * door into the same state -- shared with the constructor rather than
	 * restated, so the two cannot drift.
	 */
	if (luaext_config_refuse_hooks_with_limits(
			luaext_has_cap(&sandbox->policy, LUAEXT_CAP_DEBUG_HOOKS), limits.cpu_ns != 0,
			limits.wall_ns != 0)) {
		RETURN_THROWS();
	}

	if (!luaext_timers_set_cpu_limit(sandbox, limits.cpu_ns) ||
		!luaext_timers_set_wall_limit(sandbox, limits.wall_ns)) {
		RETURN_THROWS();
	}

	sandbox->policy.limits = limits;

	/*
	 * A ceiling below current usage is accepted and unwinds nothing: the next
	 * allocation or write that would exceed it is refused instead. See
	 * luaext_alloc_set_limit().
	 */
	luaext_alloc_set_limit(sandbox, limits.memory_bytes);
	luaext_output_set_limit(sandbox, limits.output_bytes);
}

ZEND_METHOD(DevelopGravity_LuaExt_Sandbox, limits)
{
	const luaext_sandbox *sandbox;

	ZEND_PARSE_PARAMETERS_NONE();

	sandbox = Z_LUAEXT_SANDBOX_P(ZEND_THIS);

	if (!luaext_sandbox_check_usable(sandbox)) {
		RETURN_THROWS();
	}

	luaext_config_limits_create(&sandbox->policy.limits, return_value);
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
	if (sandbox->owner_thread == luaext_thread_self() && !luaext_sandbox_check_open(sandbox)) {
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

	/*
	 * NOT the _OR_NULL variant: the stub declares a non-nullable ProfilerUnit,
	 * and for an internal function ZPP is the only thing that enforces it --
	 * the arginfo is never consulted at call time. `unit` still arrives NULL
	 * when the argument is simply omitted, which is what the default below
	 * answers.
	 */
	ZEND_PARSE_PARAMETERS_START(0, 1)
	Z_PARAM_OPTIONAL
	Z_PARAM_OBJECT_OF_CLASS(unit, luaext_ce_profiler_unit)
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
