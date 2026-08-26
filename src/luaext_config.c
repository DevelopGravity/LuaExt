/*
 * luaext — configuration value objects and the policy they resolve to.
 *
 * Capabilities, Limits, VfsQuota and SandboxConfig are how a host says what a
 * script may do. They are immutable: every field is committed once, by a
 * constructor, and the only way to derive a changed one is with(), which builds
 * a new object and leaves the original alone.
 *
 * The interesting work is luaext_config_resolve(). It flattens a SandboxConfig
 * into a luaext_policy — the form the interpreter consults on the hot path —
 * and it is where a configuration that cannot be satisfied is refused. That
 * happens at construction rather than at the point of use so a host learns on
 * the line that built the bad configuration, not several calls later.
 */

#include "luaext_config.h"

#include "luaext_timers.h"

#include <Zend/zend_closures.h>
#include <Zend/zend_enum.h>
#include <Zend/zend_exceptions.h>
#include <Zend/zend_execute.h>

/* -------------------------------------------------------------------------
 * Defaults
 *
 * These mirror the defaults declared in stubs/luaext.stub.php, which the
 * generated arginfo applies to the constructors. They are repeated in C
 * because SandboxConfig::$limits and SandboxConfig::$vfsQuota may be null, and
 * null means "the defaults" without an object ever being built to hold them.
 *
 * tests/01-basic/config-defaults.phpt pins the PHP side to the same numbers, so
 * the two copies cannot drift apart unnoticed.
 * ---------------------------------------------------------------------- */

#define LUAEXT_DEFAULT_MEMORY_BYTES ((size_t)33554432)
#define LUAEXT_DEFAULT_CPU_NS ((uint64_t)1000000000)
#define LUAEXT_DEFAULT_WALL_NS ((uint64_t)5000000000)
#define LUAEXT_DEFAULT_OUTPUT_BYTES ((size_t)1048576)
#define LUAEXT_DEFAULT_MAX_LIVE_COROUTINES 64
#define LUAEXT_DEFAULT_MAX_COROUTINE_DEPTH 16
#define LUAEXT_DEFAULT_MAX_CALL_DEPTH 200
#define LUAEXT_DEFAULT_MAX_MODULES 64
#define LUAEXT_DEFAULT_MAX_REQUIRE_DEPTH 16
#define LUAEXT_DEFAULT_MAX_STRING_LENGTH ((size_t)67108864)
#define LUAEXT_DEFAULT_MAX_SOURCE_BYTES ((size_t)1048576)
#define LUAEXT_DEFAULT_MAX_CONVERSION_DEPTH 64

#define LUAEXT_DEFAULT_MAX_OPEN_HANDLES 16
#define LUAEXT_DEFAULT_MAX_FILE_BYTES ((size_t)1048576)
#define LUAEXT_DEFAULT_MAX_TOTAL_BYTES ((size_t)8388608)
#define LUAEXT_DEFAULT_MAX_FILES 128
#define LUAEXT_DEFAULT_MAX_OPERATIONS 10000
#define LUAEXT_DEFAULT_MAX_PATH_LENGTH 255
#define LUAEXT_DEFAULT_MAX_PATH_DEPTH 16

#define LUAEXT_DEFAULT_OUTPUT_CHUNK_BYTES 8192

/* -------------------------------------------------------------------------
 * Property access
 *
 * Properties are addressed by name rather than by slot index. The generated
 * arginfo decides the slot order, and a silent disagreement between that order
 * and this file would store every field one place to the left — a class of bug
 * no test would obviously catch. A name lookup costs one hash probe, and these
 * objects are built a handful of times per request, not per instruction.
 * ---------------------------------------------------------------------- */

/* Read a declared property. The slot, not a copy: callers only inspect it. */
static zval *luaext_config_get(zend_object *object, const char *name, size_t name_length)
{
	const zend_property_info *info =
		zend_hash_str_find_ptr(&object->ce->properties_info, name, name_length);

	/* Every name used in this file is declared in stubs/luaext.stub.php. */
	LUAEXT_ASSERT(info != NULL);

	return OBJ_PROP(object, info->offset);
}

/*
 * Commit a declared property, taking ownership of `value`. Only ever called on
 * a freshly built object, so the slot is still uninitialised.
 */
static void luaext_config_set(zend_object *object, const char *name, size_t name_length,
							  zval *value)
{
	zval *slot = luaext_config_get(object, name, name_length);

	LUAEXT_ASSERT(Z_TYPE_P(slot) == IS_UNDEF);

	ZVAL_COPY_VALUE(slot, value);
}

/* Replace a declared property of a mutable object, taking ownership of `value`. */
static void luaext_config_assign(zend_object *object, const char *name, size_t name_length,
								 zval *value)
{
	zval *slot = luaext_config_get(object, name, name_length);

	zval_ptr_dtor(slot);
	ZVAL_COPY_VALUE(slot, value);
}

#define LUAEXT_GET(object, name) luaext_config_get((object), "" name, sizeof(name) - 1)
#define LUAEXT_SET(object, name, value)                                                            \
	luaext_config_set((object), "" name, sizeof(name) - 1, (value))
#define LUAEXT_ASSIGN(object, name, value)                                                         \
	luaext_config_assign((object), "" name, sizeof(name) - 1, (value))

static bool luaext_config_get_bool(zend_object *object, const char *name, size_t name_length)
{
	return Z_TYPE_P(luaext_config_get(object, name, name_length)) == IS_TRUE;
}

#define LUAEXT_GET_BOOL(object, name) luaext_config_get_bool((object), "" name, sizeof(name) - 1)

/*
 * Read an object-typed property, or NULL when it holds null. Every caller
 * treats null as "the default for this field", so the two collapse here.
 */
static zend_object *luaext_config_get_object(zend_object *object, const char *name,
											 size_t name_length)
{
	const zval *value = luaext_config_get(object, name, name_length);

	return Z_TYPE_P(value) == IS_OBJECT ? Z_OBJ_P(value) : NULL;
}

#define LUAEXT_GET_OBJECT(object, name)                                                            \
	luaext_config_get_object((object), "" name, sizeof(name) - 1)

/*
 * Refuse a second __construct() on an object that already has its fields.
 * Internal constructors write property slots directly, so the readonly guard
 * the engine applies to userland assignment never sees them: without this,
 * $config->__construct() would overwrite committed values and leak whatever
 * they pointed at.
 */
static bool luaext_config_reject_reconstruction(zend_object *object)
{
	if (object->ce->default_properties_count > 0 && Z_TYPE_P(OBJ_PROP_NUM(object, 0)) != IS_UNDEF) {
		zend_throw_exception_ex(luaext_ce_configuration_error, 0,
								"%s is immutable and has already been constructed. Build a "
								"separate object, or derive one with with().",
								ZSTR_VAL(object->ce->name));
		return false;
	}

	return true;
}

/* -------------------------------------------------------------------------
 * Capabilities
 * ---------------------------------------------------------------------- */

/*
 * The bit each boolean capability contributes to luaext_policy::caps.
 * osEnvAllowList is deliberately absent: it is a list, not a flag, and the
 * subsystem that reads it takes it from the Capabilities object directly.
 */
typedef struct {
	const char *name;
	size_t name_length;
	uint32_t bit;
} luaext_capability_bit;

#define LUAEXT_CAPABILITY_BIT(name, bit) {"" name, sizeof(name) - 1, (bit)}

static const luaext_capability_bit luaext_capability_bits[] = {
	LUAEXT_CAPABILITY_BIT("loadBytecode", LUAEXT_CAP_LOAD_BYTECODE),
	LUAEXT_CAPABILITY_BIT("compileAtRuntime", LUAEXT_CAP_COMPILE_AT_RUNTIME),
	LUAEXT_CAPABILITY_BIT("dumpBytecode", LUAEXT_CAP_DUMP_BYTECODE),
	LUAEXT_CAPABILITY_BIT("require", LUAEXT_CAP_REQUIRE),
	LUAEXT_CAPABILITY_BIT("vfs", LUAEXT_CAP_VFS),
	LUAEXT_CAPABILITY_BIT("vfsWrite", LUAEXT_CAP_VFS_WRITE),
	LUAEXT_CAPABILITY_BIT("coroutines", LUAEXT_CAP_COROUTINES),
	LUAEXT_CAPABILITY_BIT("osTime", LUAEXT_CAP_OS_TIME),
	LUAEXT_CAPABILITY_BIT("osEnv", LUAEXT_CAP_OS_ENV),
	LUAEXT_CAPABILITY_BIT("debugTraceback", LUAEXT_CAP_DEBUG_TRACEBACK),
	LUAEXT_CAPABILITY_BIT("debugIntrospect", LUAEXT_CAP_DEBUG_INTROSPECT),
	LUAEXT_CAPABILITY_BIT("debugMutate", LUAEXT_CAP_DEBUG_MUTATE),
	LUAEXT_CAPABILITY_BIT("debugHooks", LUAEXT_CAP_DEBUG_HOOKS),
	LUAEXT_CAPABILITY_BIT("utf8", LUAEXT_CAP_UTF8),
	LUAEXT_CAPABILITY_BIT("gcControl", LUAEXT_CAP_GC_CONTROL),
	LUAEXT_CAPABILITY_BIT("warn", LUAEXT_CAP_WARN),
};

#define LUAEXT_CAPABILITY_BIT_COUNT                                                                \
	(sizeof(luaext_capability_bits) / sizeof(luaext_capability_bits[0]))

/* A null Capabilities means the untrusted baseline, exactly as `new Capabilities()` does. */
static uint32_t luaext_config_caps(zend_object *capabilities)
{
	uint32_t caps = 0;
	size_t index;

	if (capabilities == NULL) {
		return LUAEXT_CAPS_UNTRUSTED;
	}

	for (index = 0; index < LUAEXT_CAPABILITY_BIT_COUNT; index++) {
		const luaext_capability_bit *capability = &luaext_capability_bits[index];

		if (luaext_config_get_bool(capabilities, capability->name, capability->name_length)) {
			caps |= capability->bit;
		}
	}

	return caps;
}

/*
 * Which libraries a sandbox is given.
 *
 * A bit here means "install this library", which is NOT the same claim as "the
 * capability behind it is granted". Some capabilities can only be honoured by a
 * filtered or wrapped table, and for those the mapping from capability to bit is
 * deliberately not one to one. Read the two exceptions below before adding the
 * obvious-looking line that completes the pattern.
 *
 * base, table, string and math are always installed; the individual members of
 * them that reach outside the sandbox are replaced by the library policy rather
 * than withheld here.
 */
static uint32_t luaext_config_open_libs(uint32_t caps)
{
	uint32_t libs = LUAEXT_LIB_BASE | LUAEXT_LIB_TABLE | LUAEXT_LIB_STR | LUAEXT_LIB_MATH;

	if ((caps & LUAEXT_CAP_UTF8) != 0) {
		libs |= LUAEXT_LIB_UTF8;
	}

	/*
	 * Exception 1: LUAEXT_LIB_CORO means our wrapper, never upstream's
	 * luaopen_coroutine. The wrapper is what caps live coroutines and stops
	 * resume() from returning a fatal error as `false, err` instead of
	 * re-raising it, so upstream's table would quietly undo two guarantees.
	 *
	 * So the bit is deliberately NOT set here, even though the capability
	 * defaults to true: setting it would name a library that no opener
	 * installs, which is worse than absent because it reads as done. A sandbox
	 * therefore has no coroutine table at all, and Sandbox::features() reports
	 * the capability as unimplemented rather than letting it look granted.
	 */

	/*
	 * Exception 2: LUAEXT_LIB_DEBUG stays clear no matter which debug*
	 * capability is set, and there is deliberately no `if` for it here.
	 *
	 * debugTraceback is part of LUAEXT_CAPS_UNTRUSTED, so setting the bit to
	 * satisfy it would open upstream's whole debug library to *every* untrusted
	 * sandbox: debug.sethook, which lets a script displace the hook the CPU
	 * limit is delivered through, and debug.setupvalue, which reaches straight
	 * out of the sandbox. The debug table a script sees is assembled from the
	 * individual debug* capabilities by the library policy; until that exists,
	 * a sandbox gets no debug table at all, which is the safe direction.
	 */

	return libs;
}

void luaext_config_capabilities_create(uint32_t caps, zval *out)
{
	zend_object *object;
	size_t index;
	zval value;

	object_init_ex(out, luaext_ce_capabilities);
	object = Z_OBJ_P(out);

	for (index = 0; index < LUAEXT_CAPABILITY_BIT_COUNT; index++) {
		const luaext_capability_bit *capability = &luaext_capability_bits[index];

		ZVAL_BOOL(&value, (caps & capability->bit) != 0);
		luaext_config_set(object, capability->name, capability->name_length, &value);
	}

	/*
	 * A bitset cannot carry a list, so the allow list is empty here. The
	 * presets have nothing to put in it, and with() copies the source object's
	 * fields rather than round-tripping them through a bitset.
	 */
	ZVAL_EMPTY_ARRAY(&value);
	LUAEXT_SET(object, "osEnvAllowList", &value);
}

/* -------------------------------------------------------------------------
 * Limits and quotas
 *
 * Throughout luaext_policy, 0 means "no limit", which is what a null field
 * resolves to. A negative one is refused rather than converted: (size_t)-1 is
 * SIZE_MAX, so a typo would quietly become the widest possible budget.
 * ---------------------------------------------------------------------- */

/*
 * A declared property that was never written. Unreachable through the public
 * API, because every constructor commits every field — but
 * ReflectionClass::newInstanceWithoutConstructor() produces exactly this, and
 * reading whatever the slot happens to contain is not an option here.
 */
static bool luaext_config_reject_uninitialised(const zval *value, const char *property)
{
	if (EXPECTED(Z_TYPE_P(value) != IS_UNDEF)) {
		return false;
	}

	zend_throw_exception_ex(luaext_ce_configuration_error, 0,
							"%s was never initialised, so there is no limit to read. Build the "
							"object with its constructor rather than around it.",
							property);

	return true;
}

static void luaext_config_reject_negative(const char *property, zend_long value)
{
	zend_throw_exception_ex(luaext_ce_configuration_error, 0,
							"%s is " ZEND_LONG_FMT ", and a limit cannot be negative. Pass 0 (or "
							"null, where the type allows it) to lift the limit, or a positive "
							"value to set one.",
							property, value);
}

static bool luaext_config_size(const zval *value, const char *property, size_t *out)
{
	zend_long number;

	if (luaext_config_reject_uninitialised(value, property)) {
		return false;
	}

	if (Z_TYPE_P(value) == IS_NULL) {
		*out = 0;
		return true;
	}

	LUAEXT_ASSERT(Z_TYPE_P(value) == IS_LONG);
	number = Z_LVAL_P(value);

	if (number < 0) {
		luaext_config_reject_negative(property, number);
		return false;
	}

	*out = (size_t)number;

	return true;
}

static bool luaext_config_count(const zval *value, const char *property, uint32_t *out)
{
	zend_long number;

	if (luaext_config_reject_uninitialised(value, property)) {
		return false;
	}

	if (Z_TYPE_P(value) == IS_NULL) {
		*out = 0;
		return true;
	}

	LUAEXT_ASSERT(Z_TYPE_P(value) == IS_LONG);
	number = Z_LVAL_P(value);

	if (number < 0) {
		luaext_config_reject_negative(property, number);
		return false;
	}

	/* A count wider than the counter is as good as no limit; saturate rather
	 * than wrap, which would turn a huge ceiling into a tiny one. */
	*out = (uint64_t)number > (uint64_t)UINT32_MAX ? UINT32_MAX : (uint32_t)number;

	return true;
}

static bool luaext_config_duration(const zval *value, const char *property, uint64_t *out)
{
	double seconds;

	if (luaext_config_reject_uninitialised(value, property)) {
		return false;
	}

	if (Z_TYPE_P(value) == IS_NULL) {
		*out = 0;
		return true;
	}

	LUAEXT_ASSERT(Z_TYPE_P(value) == IS_DOUBLE);
	seconds = Z_DVAL_P(value);

	/* Phrased so NaN fails too: it compares false against everything. */
	if (!(seconds >= 0.0)) {
		zend_throw_exception_ex(luaext_ce_configuration_error, 0,
								"%s is %G, which is not a number of seconds a deadline can be set "
								"from. Pass null (or 0.0) to lift the limit, or a positive number "
								"of seconds to set one.",
								property, seconds);
		return false;
	}

	/*
	 * Saturate rather than cast. A deadline is held in nanoseconds, so anything
	 * past LUAEXT_LIMIT_MAX_SECONDS cannot be represented -- and converting an
	 * out-of-range double to an integer is undefined behaviour whose result can
	 * be anything, including zero. Zero is how this API spells "no ceiling", so
	 * the wrong answer here is not merely wrong, it is a huge limit silently
	 * becoming none at all.
	 *
	 * Saturating keeps the direction honest: a ceiling nobody will reach stays
	 * a ceiling nobody will reach.
	 */
	if (seconds >= LUAEXT_LIMIT_MAX_SECONDS) {
		*out = UINT64_MAX;
		return true;
	}

	*out = (uint64_t)(seconds * 1e9);

	return true;
}

/*
 * Enum cases are singletons, so identity is the whole comparison. Anything that
 * is not recognisably Truncate resolves to Fail, which is the direction that
 * cannot be talked past: a script hitting its output budget is stopped rather
 * than quietly allowed to keep going.
 */
static uint8_t luaext_config_overflow(const zval *value)
{
	return Z_TYPE_P(value) == IS_OBJECT &&
				   Z_OBJ_P(value) ==
					   zend_enum_get_case_cstr(luaext_ce_overflow_behavior, "Truncate")
			   ? (uint8_t)LUAEXT_OVERFLOW_TRUNCATE
			   : (uint8_t)LUAEXT_OVERFLOW_FAIL;
}

static void luaext_config_default_limits(luaext_limits *out)
{
	out->memory_bytes = LUAEXT_DEFAULT_MEMORY_BYTES;
	out->cpu_ns = LUAEXT_DEFAULT_CPU_NS;
	out->wall_ns = LUAEXT_DEFAULT_WALL_NS;
	out->output_bytes = LUAEXT_DEFAULT_OUTPUT_BYTES;
	out->output_overflow = (uint8_t)LUAEXT_OVERFLOW_FAIL;
	out->max_live_coroutines = LUAEXT_DEFAULT_MAX_LIVE_COROUTINES;
	out->max_coroutine_depth = LUAEXT_DEFAULT_MAX_COROUTINE_DEPTH;
	out->max_call_depth = LUAEXT_DEFAULT_MAX_CALL_DEPTH;
	out->max_modules = LUAEXT_DEFAULT_MAX_MODULES;
	out->max_require_depth = LUAEXT_DEFAULT_MAX_REQUIRE_DEPTH;
	out->max_string_length = LUAEXT_DEFAULT_MAX_STRING_LENGTH;
	out->max_source_bytes = LUAEXT_DEFAULT_MAX_SOURCE_BYTES;
	out->max_conversion_depth = LUAEXT_DEFAULT_MAX_CONVERSION_DEPTH;
}

static bool luaext_config_limits(zend_object *limits, luaext_limits *out)
{
	if (limits == NULL) {
		luaext_config_default_limits(out);
		return true;
	}

	out->output_overflow = luaext_config_overflow(LUAEXT_GET(limits, "outputOverflow"));

	return luaext_config_size(LUAEXT_GET(limits, "memoryBytes"), "Limits::$memoryBytes",
							  &out->memory_bytes) &&
		   luaext_config_duration(LUAEXT_GET(limits, "cpuSeconds"), "Limits::$cpuSeconds",
								  &out->cpu_ns) &&
		   luaext_config_duration(LUAEXT_GET(limits, "wallClockSeconds"),
								  "Limits::$wallClockSeconds", &out->wall_ns) &&
		   luaext_config_size(LUAEXT_GET(limits, "outputBytes"), "Limits::$outputBytes",
							  &out->output_bytes) &&
		   luaext_config_count(LUAEXT_GET(limits, "maxLiveCoroutines"),
							   "Limits::$maxLiveCoroutines", &out->max_live_coroutines) &&
		   luaext_config_count(LUAEXT_GET(limits, "maxCoroutineDepth"),
							   "Limits::$maxCoroutineDepth", &out->max_coroutine_depth) &&
		   luaext_config_count(LUAEXT_GET(limits, "maxCallDepth"), "Limits::$maxCallDepth",
							   &out->max_call_depth) &&
		   luaext_config_count(LUAEXT_GET(limits, "maxModules"), "Limits::$maxModules",
							   &out->max_modules) &&
		   luaext_config_count(LUAEXT_GET(limits, "maxRequireDepth"), "Limits::$maxRequireDepth",
							   &out->max_require_depth) &&
		   luaext_config_size(LUAEXT_GET(limits, "maxStringLength"), "Limits::$maxStringLength",
							  &out->max_string_length) &&
		   luaext_config_size(LUAEXT_GET(limits, "maxSourceBytes"), "Limits::$maxSourceBytes",
							  &out->max_source_bytes) &&
		   luaext_config_count(LUAEXT_GET(limits, "maxConversionDepth"),
							   "Limits::$maxConversionDepth", &out->max_conversion_depth);
}

static void luaext_config_default_vfs_quota(luaext_vfs_quota *out)
{
	out->max_open_handles = LUAEXT_DEFAULT_MAX_OPEN_HANDLES;
	out->max_file_bytes = LUAEXT_DEFAULT_MAX_FILE_BYTES;
	out->max_total_bytes = LUAEXT_DEFAULT_MAX_TOTAL_BYTES;
	out->max_files = LUAEXT_DEFAULT_MAX_FILES;
	out->max_operations = LUAEXT_DEFAULT_MAX_OPERATIONS;
	out->max_path_length = LUAEXT_DEFAULT_MAX_PATH_LENGTH;
	out->max_path_depth = LUAEXT_DEFAULT_MAX_PATH_DEPTH;
	out->bill_wall_time = false;
}

static bool luaext_config_vfs_quota(zend_object *quota, luaext_vfs_quota *out)
{
	if (quota == NULL) {
		luaext_config_default_vfs_quota(out);
		return true;
	}

	out->bill_wall_time = LUAEXT_GET_BOOL(quota, "billWallTime");

	return luaext_config_count(LUAEXT_GET(quota, "maxOpenHandles"), "VfsQuota::$maxOpenHandles",
							   &out->max_open_handles) &&
		   luaext_config_size(LUAEXT_GET(quota, "maxFileBytes"), "VfsQuota::$maxFileBytes",
							  &out->max_file_bytes) &&
		   luaext_config_size(LUAEXT_GET(quota, "maxTotalBytes"), "VfsQuota::$maxTotalBytes",
							  &out->max_total_bytes) &&
		   luaext_config_count(LUAEXT_GET(quota, "maxFiles"), "VfsQuota::$maxFiles",
							   &out->max_files) &&
		   luaext_config_count(LUAEXT_GET(quota, "maxOperations"), "VfsQuota::$maxOperations",
							   &out->max_operations) &&
		   luaext_config_count(LUAEXT_GET(quota, "maxPathLength"), "VfsQuota::$maxPathLength",
							   &out->max_path_length) &&
		   luaext_config_count(LUAEXT_GET(quota, "maxPathDepth"), "VfsQuota::$maxPathDepth",
							   &out->max_path_depth);
}

/* -------------------------------------------------------------------------
 * Refusals
 *
 * Three combinations describe a sandbox that cannot keep the promises the rest
 * of the extension makes about it. Each is refused where the configuration is
 * assembled, with a message that says what to do about it: a host reading the
 * exception should not have to consult the manual to get moving again.
 * ---------------------------------------------------------------------- */

static bool luaext_config_check(zend_object *capabilities, zend_object *limits,
								zend_object *filesystem, bool seed_is_fixed, bool deterministic)
{
	bool debug_hooks = capabilities != NULL && LUAEXT_GET_BOOL(capabilities, "debugHooks");
	bool vfs = capabilities != NULL && LUAEXT_GET_BOOL(capabilities, "vfs");
	bool vfs_write = capabilities != NULL && LUAEXT_GET_BOOL(capabilities, "vfsWrite");
	bool has_cpu_limit;
	bool has_wall_limit;

	if (limits == NULL) {
		/* The default Limits carries cpuSeconds = 1.0 and wallClockSeconds. */
		has_cpu_limit = true;
		has_wall_limit = true;
	} else {
		const zval *cpu_seconds = LUAEXT_GET(limits, "cpuSeconds");
		const zval *wall_seconds = LUAEXT_GET(limits, "wallClockSeconds");

		has_cpu_limit = Z_TYPE_P(cpu_seconds) == IS_DOUBLE && Z_DVAL_P(cpu_seconds) > 0.0;
		has_wall_limit = Z_TYPE_P(wall_seconds) == IS_DOUBLE && Z_DVAL_P(wall_seconds) > 0.0;
	}

	/*
	 * Debug hooks are per-coroutine and there is only one of them. A script
	 * that can call debug.sethook() can therefore replace the hook the limits
	 * are delivered through, which makes the pair unsatisfiable rather than
	 * merely unwise: whichever is configured, the script decides.
	 *
	 * The wall-clock limit is covered too, and that is not an over-reach. The
	 * watchdog thread only ever raises a FLAG; the flag is turned into a stopped
	 * script by the same count hook, so displacing the hook defeats both limits
	 * and not just the one whose name mentions the CPU.
	 */
	if (debug_hooks && (has_cpu_limit || has_wall_limit)) {
		zend_throw_exception(
			luaext_ce_configuration_error,
			"The debugHooks capability cannot be combined with a CPU or wall-clock limit: a "
			"script that can call debug.sethook() replaces the interpreter hook BOTH limits "
			"are delivered through -- the watchdog thread only raises a flag, and that hook "
			"is what turns the flag into a stopped script -- so either limit would stop being "
			"enforced the moment the script chose to. Either drop debugHooks, or set both "
			"Limits::$cpuSeconds and Limits::$wallClockSeconds to null and accept that this "
			"sandbox cannot be bounded in time.",
			0);
		return false;
	}

	/*
	 * The seed is Lua's string hash seed. Pinning it makes a run reproducible
	 * and simultaneously makes hash flooding reproducible, so it is only
	 * accepted from a host that has said out loud it wants determinism.
	 */
	if (seed_is_fixed && !deterministic) {
		zend_throw_exception(
			luaext_ce_configuration_error,
			"A fixed SandboxConfig::$seed pins Lua's string hash seed, which forfeits the "
			"hash-flooding protection a random seed provides, so it has to be asked for "
			"explicitly: pass deterministic: true alongside it if this sandbox runs code you "
			"trust, or leave $seed null to draw one from the system CSPRNG.",
			0);
		return false;
	}

	/*
	 * The vfs capability installs an io/os file API. Without a backend there is
	 * nothing behind it, and every call a script made would fail at runtime for
	 * a reason the host could have known at construction.
	 */
	/*
	 * Write is an aspect of filesystem access, not a capability that stands on
	 * its own: vfsWrite widens what vfs may do, so granting it alone describes a
	 * sandbox that can modify a store it cannot open. Refused rather than quietly
	 * turning vfs on, because a host that gets back more access than it asked for
	 * has been surprised in the dangerous direction -- and this file's whole habit
	 * is to reject what it cannot satisfy at the point it is configured.
	 */
	if (vfs_write && !vfs) {
		zend_throw_exception(
			luaext_ce_configuration_error,
			"The vfsWrite capability widens vfs rather than replacing it, so it cannot be "
			"granted on its own: this configuration asks for a sandbox that may modify a "
			"filesystem it may not read. Pass $capabilities->with(vfs: true, vfsWrite: true) "
			"for read and write, or drop vfsWrite for read-only access.",
			0);
		return false;
	}

	/*
	 * Both VFS toggles name access to the host's store, so neither is satisfiable
	 * without one.
	 */
	if ((vfs || vfs_write) && filesystem == NULL) {
		zend_throw_exception(
			luaext_ce_configuration_error,
			"The vfs and vfsWrite capabilities need a backing store, but "
			"SandboxConfig::$filesystem is null. Pass an object implementing "
			"DevelopGravity\\LuaExt\\FileSystem, or turn them off with "
			"$capabilities->with(vfs: false, vfsWrite: false). Capabilities::trusted() "
			"enables vfs, so a trusted sandbox has to supply one too.",
			0);
		return false;
	}

	return true;
}

/*
 * The whole of resolution, expressed over the individual settings rather than
 * over a SandboxConfig.
 *
 * SandboxConfig::__construct() calls this with its own arguments, before it has
 * written a single property: an unsatisfiable configuration then never produces
 * an object at all, half-built or otherwise. luaext_config_resolve() calls it
 * with the settings read back off a finished object.
 */
static bool luaext_config_resolve_parts(zend_object *capabilities, zend_object *limits,
										zend_object *vfs_quota, zend_object *filesystem,
										bool seed_is_fixed, zend_long seed, bool deterministic,
										luaext_policy *policy)
{
	memset(policy, 0, sizeof(*policy));

	if (!luaext_config_check(capabilities, limits, filesystem, seed_is_fixed, deterministic)) {
		return false;
	}

	policy->caps = luaext_config_caps(capabilities);
	policy->open_libs = luaext_config_open_libs(policy->caps);

	/*
	 * Carried, not merely validated. The check above refuses a fixed seed
	 * unless the host also asked for determinism; without propagating it the
	 * refusal would guard a setting nothing ever read, and deterministic: true
	 * would produce a different hash seed on every construction.
	 */
	policy->seed_is_fixed = seed_is_fixed;
	policy->seed = seed_is_fixed ? (uint64_t)seed : 0;

	return luaext_config_limits(limits, &policy->limits) &&
		   luaext_config_vfs_quota(vfs_quota, &policy->vfs_quota);
}

bool luaext_config_resolve(zval *config, luaext_policy *policy)
{
	zend_object *object;

	zval *seed;

	if (config == NULL || Z_TYPE_P(config) != IS_OBJECT) {
		/* No configuration at all is the untrusted baseline with default limits. */
		return luaext_config_resolve_parts(NULL, NULL, NULL, NULL, false, 0, false, policy);
	}

	object = Z_OBJ_P(config);
	LUAEXT_ASSERT(object->ce == luaext_ce_sandbox_config);

	seed = LUAEXT_GET(object, "seed");

	return luaext_config_resolve_parts(
		LUAEXT_GET_OBJECT(object, "capabilities"), LUAEXT_GET_OBJECT(object, "limits"),
		LUAEXT_GET_OBJECT(object, "vfsQuota"), LUAEXT_GET_OBJECT(object, "filesystem"),
		Z_TYPE_P(seed) == IS_LONG, Z_TYPE_P(seed) == IS_LONG ? Z_LVAL_P(seed) : 0,
		LUAEXT_GET_BOOL(object, "deterministic"), policy);
}

/* -------------------------------------------------------------------------
 * with()
 *
 * One engine for all four value objects: copy every declared property, then
 * replace the ones the caller named. Nothing is written back to the source, so
 * a with() chain leaves every intermediate object exactly as it was.
 * ---------------------------------------------------------------------- */

static void luaext_config_throw_unknown(const zend_class_entry *ce, const zend_string *name)
{
	smart_str candidates = {0};
	uint32_t count = (uint32_t)ce->default_properties_count;
	uint32_t index;

	for (index = 0; index < count; index++) {
		if (index > 0) {
			smart_str_appends(&candidates, ", ");
		}

		smart_str_append(&candidates, ce->properties_info_table[index]->name);
	}

	smart_str_0(&candidates);

	zend_throw_exception_ex(luaext_ce_configuration_error, 0,
							"%s::with(): \"%s\" is not a property of %s, so there is nothing for "
							"it to replace. Name one of: %s.",
							ZSTR_VAL(ce->name), name != NULL ? ZSTR_VAL(name) : "",
							ZSTR_VAL(ce->name), ZSTR_VAL(candidates.s));

	smart_str_free(&candidates);
}

/*
 * with() takes named arguments only. A positional form would have to mean
 * "the Nth declared property", which makes the order the stub happens to
 * declare fields in part of the public API — reorder two lines and every
 * caller silently changes a different field.
 */
static bool luaext_config_reject_positional(const zend_class_entry *ce, uint32_t count)
{
	if (count == 0) {
		return true;
	}

	zend_throw_exception_ex(
		luaext_ce_configuration_error, 0,
		"%s::with() takes named arguments only, and was given %d positional one%s. Each "
		"override has to say which field it replaces, because a positional form would depend "
		"on the order the properties happen to be declared in. Write with(name: $value).",
		ZSTR_VAL(ce->name), (int)count, count == 1 ? "" : "s");

	return false;
}

bool luaext_config_with(zend_class_entry *ce, zend_object *source, HashTable *named, zval *out)
{
	zend_object *copy;
	uint32_t count = (uint32_t)ce->default_properties_count;
	uint32_t index;
	zend_string *key;
	zval *override;

	object_init_ex(out, ce);
	copy = Z_OBJ_P(out);

	/* The declared defaults are all undefined, so nothing is being overwritten. */
	for (index = 0; index < count; index++) {
		ZVAL_COPY(OBJ_PROP_NUM(copy, index), OBJ_PROP_NUM(source, index));
	}

	if (named == NULL) {
		return true;
	}

	ZEND_HASH_FOREACH_STR_KEY_VAL(named, key, override)
	{
		zend_property_info *info =
			key != NULL ? zend_hash_find_ptr(&ce->properties_info, key) : NULL;
		zval *slot;
		zval value;

		if (info == NULL || (info->flags & ZEND_ACC_STATIC) != 0) {
			luaext_config_throw_unknown(ce, key);
			goto failed;
		}

		/*
		 * Copy before verifying: zend_verify_property_type() coerces in place
		 * under weak typing, and the named-argument table is the caller's.
		 */
		ZVAL_COPY_DEREF(&value, override);

		if (!zend_verify_property_type(info, &value, ZEND_ARG_USES_STRICT_TYPES())) {
			zval_ptr_dtor(&value);
			goto failed;
		}

		slot = OBJ_PROP(copy, info->offset);
		zval_ptr_dtor(slot);
		ZVAL_COPY_VALUE(slot, &value);
	}
	ZEND_HASH_FOREACH_END();

	return true;

failed:
	/*
	 * Null rather than undefined: `out` is the caller's return_value, and the
	 * engine still owns that slot once an exception is pending.
	 */
	zval_ptr_dtor(out);
	ZVAL_NULL(out);

	return false;
}

/*
 * The three value objects whose with() is nothing but the engine. SandboxConfig
 * is written out separately: it re-runs the refusals over the derived object,
 * because with() can reach an unsatisfiable combination the source did not have.
 */
#define LUAEXT_CONFIG_WITH_METHOD(method_class, class_entry)                                       \
	ZEND_METHOD(method_class, with)                                                                \
	{                                                                                              \
		zval *positional = NULL;                                                                   \
		uint32_t positional_count = 0;                                                             \
		HashTable *named = NULL;                                                                   \
                                                                                                   \
		ZEND_PARSE_PARAMETERS_START(0, -1)                                                         \
		Z_PARAM_VARIADIC_WITH_NAMED(positional, positional_count, named)                           \
		ZEND_PARSE_PARAMETERS_END();                                                               \
                                                                                                   \
		(void)positional;                                                                          \
                                                                                                   \
		if (!luaext_config_reject_positional(class_entry, positional_count) ||                     \
			!luaext_config_with(class_entry, Z_OBJ_P(ZEND_THIS), named, return_value)) {           \
			RETURN_THROWS();                                                                       \
		}                                                                                          \
	}

/* -------------------------------------------------------------------------
 * Capabilities
 * ---------------------------------------------------------------------- */

ZEND_METHOD(DevelopGravity_LuaExt_Capabilities, __construct)
{
	bool load_bytecode = false;
	bool compile_at_runtime = false;
	bool dump_bytecode = false;
	bool require_modules = false;
	bool vfs = false;
	bool vfs_write = false;
	bool coroutines = true;
	bool os_time = true;
	bool os_env = false;
	/*
	 * Taken as a zval rather than a HashTable: an empty array literal, and the
	 * declared "[]" default, are the immutable zend_empty_array, and building a
	 * refcounted zval around one would increment a refcount that lives in
	 * read-only memory. ZVAL_COPY carries the source's own type info instead.
	 */
	zval *os_env_allow_list = NULL;
	bool debug_traceback = true;
	bool debug_introspect = false;
	bool debug_mutate = false;
	bool debug_hooks = false;
	bool utf8 = true;
	bool gc_control = false;
	bool warn = false;
	zend_object *object;
	zval value;

	ZEND_PARSE_PARAMETERS_START(0, 17)
	Z_PARAM_OPTIONAL
	Z_PARAM_BOOL(load_bytecode)
	Z_PARAM_BOOL(compile_at_runtime)
	Z_PARAM_BOOL(dump_bytecode)
	Z_PARAM_BOOL(require_modules)
	Z_PARAM_BOOL(vfs)
	Z_PARAM_BOOL(vfs_write)
	Z_PARAM_BOOL(coroutines)
	Z_PARAM_BOOL(os_time)
	Z_PARAM_BOOL(os_env)
	Z_PARAM_ARRAY(os_env_allow_list)
	Z_PARAM_BOOL(debug_traceback)
	Z_PARAM_BOOL(debug_introspect)
	Z_PARAM_BOOL(debug_mutate)
	Z_PARAM_BOOL(debug_hooks)
	Z_PARAM_BOOL(utf8)
	Z_PARAM_BOOL(gc_control)
	Z_PARAM_BOOL(warn)
	ZEND_PARSE_PARAMETERS_END();

	object = Z_OBJ_P(ZEND_THIS);

	if (!luaext_config_reject_reconstruction(object)) {
		RETURN_THROWS();
	}

	ZVAL_BOOL(&value, load_bytecode);
	LUAEXT_SET(object, "loadBytecode", &value);
	ZVAL_BOOL(&value, compile_at_runtime);
	LUAEXT_SET(object, "compileAtRuntime", &value);
	ZVAL_BOOL(&value, dump_bytecode);
	LUAEXT_SET(object, "dumpBytecode", &value);
	ZVAL_BOOL(&value, require_modules);
	LUAEXT_SET(object, "require", &value);
	ZVAL_BOOL(&value, vfs);
	LUAEXT_SET(object, "vfs", &value);
	ZVAL_BOOL(&value, vfs_write);
	LUAEXT_SET(object, "vfsWrite", &value);
	ZVAL_BOOL(&value, coroutines);
	LUAEXT_SET(object, "coroutines", &value);
	ZVAL_BOOL(&value, os_time);
	LUAEXT_SET(object, "osTime", &value);
	ZVAL_BOOL(&value, os_env);
	LUAEXT_SET(object, "osEnv", &value);

	if (os_env_allow_list != NULL) {
		ZVAL_COPY(&value, os_env_allow_list);
	} else {
		ZVAL_EMPTY_ARRAY(&value);
	}

	LUAEXT_SET(object, "osEnvAllowList", &value);

	ZVAL_BOOL(&value, debug_traceback);
	LUAEXT_SET(object, "debugTraceback", &value);
	ZVAL_BOOL(&value, debug_introspect);
	LUAEXT_SET(object, "debugIntrospect", &value);
	ZVAL_BOOL(&value, debug_mutate);
	LUAEXT_SET(object, "debugMutate", &value);
	ZVAL_BOOL(&value, debug_hooks);
	LUAEXT_SET(object, "debugHooks", &value);
	ZVAL_BOOL(&value, utf8);
	LUAEXT_SET(object, "utf8", &value);
	ZVAL_BOOL(&value, gc_control);
	LUAEXT_SET(object, "gcControl", &value);
	ZVAL_BOOL(&value, warn);
	LUAEXT_SET(object, "warn", &value);
}

ZEND_METHOD(DevelopGravity_LuaExt_Capabilities, untrusted)
{
	ZEND_PARSE_PARAMETERS_NONE();

	luaext_config_capabilities_create(LUAEXT_CAPS_UNTRUSTED, return_value);
}

ZEND_METHOD(DevelopGravity_LuaExt_Capabilities, trusted)
{
	ZEND_PARSE_PARAMETERS_NONE();

	luaext_config_capabilities_create(LUAEXT_CAPS_TRUSTED, return_value);
}

LUAEXT_CONFIG_WITH_METHOD(DevelopGravity_LuaExt_Capabilities, luaext_ce_capabilities)

/* -------------------------------------------------------------------------
 * Limits
 * ---------------------------------------------------------------------- */

ZEND_METHOD(DevelopGravity_LuaExt_Limits, __construct)
{
	zend_long memory_bytes = (zend_long)LUAEXT_DEFAULT_MEMORY_BYTES;
	bool memory_bytes_is_null = false;
	double cpu_seconds = 1.0;
	bool cpu_seconds_is_null = false;
	double wall_clock_seconds = 5.0;
	bool wall_clock_seconds_is_null = false;
	zend_long output_bytes = (zend_long)LUAEXT_DEFAULT_OUTPUT_BYTES;
	zend_object *output_overflow = NULL;
	zend_long max_live_coroutines = LUAEXT_DEFAULT_MAX_LIVE_COROUTINES;
	zend_long max_coroutine_depth = LUAEXT_DEFAULT_MAX_COROUTINE_DEPTH;
	zend_long max_call_depth = LUAEXT_DEFAULT_MAX_CALL_DEPTH;
	zend_long max_modules = LUAEXT_DEFAULT_MAX_MODULES;
	zend_long max_require_depth = LUAEXT_DEFAULT_MAX_REQUIRE_DEPTH;
	zend_long max_string_length = (zend_long)LUAEXT_DEFAULT_MAX_STRING_LENGTH;
	zend_long max_source_bytes = (zend_long)LUAEXT_DEFAULT_MAX_SOURCE_BYTES;
	zend_long max_conversion_depth = LUAEXT_DEFAULT_MAX_CONVERSION_DEPTH;
	zend_object *object;
	zval value;

	ZEND_PARSE_PARAMETERS_START(0, 13)
	Z_PARAM_OPTIONAL
	Z_PARAM_LONG_OR_NULL(memory_bytes, memory_bytes_is_null)
	Z_PARAM_DOUBLE_OR_NULL(cpu_seconds, cpu_seconds_is_null)
	Z_PARAM_DOUBLE_OR_NULL(wall_clock_seconds, wall_clock_seconds_is_null)
	Z_PARAM_LONG(output_bytes)
	Z_PARAM_OBJ_OF_CLASS(output_overflow, luaext_ce_overflow_behavior)
	Z_PARAM_LONG(max_live_coroutines)
	Z_PARAM_LONG(max_coroutine_depth)
	Z_PARAM_LONG(max_call_depth)
	Z_PARAM_LONG(max_modules)
	Z_PARAM_LONG(max_require_depth)
	Z_PARAM_LONG(max_string_length)
	Z_PARAM_LONG(max_source_bytes)
	Z_PARAM_LONG(max_conversion_depth)
	ZEND_PARSE_PARAMETERS_END();

	object = Z_OBJ_P(ZEND_THIS);

	if (!luaext_config_reject_reconstruction(object)) {
		RETURN_THROWS();
	}

	if (memory_bytes_is_null) {
		ZVAL_NULL(&value);
	} else {
		ZVAL_LONG(&value, memory_bytes);
	}
	LUAEXT_SET(object, "memoryBytes", &value);

	if (cpu_seconds_is_null) {
		ZVAL_NULL(&value);
	} else {
		ZVAL_DOUBLE(&value, cpu_seconds);
	}
	LUAEXT_SET(object, "cpuSeconds", &value);

	if (wall_clock_seconds_is_null) {
		ZVAL_NULL(&value);
	} else {
		ZVAL_DOUBLE(&value, wall_clock_seconds);
	}
	LUAEXT_SET(object, "wallClockSeconds", &value);

	ZVAL_LONG(&value, output_bytes);
	LUAEXT_SET(object, "outputBytes", &value);

	/*
	 * An internal function is not handed its declared default for an
	 * object-typed parameter, so the enum case is fetched here instead.
	 */
	if (output_overflow == NULL) {
		output_overflow = zend_enum_get_case_cstr(luaext_ce_overflow_behavior, "Fail");
	}

	ZVAL_OBJ_COPY(&value, output_overflow);
	LUAEXT_SET(object, "outputOverflow", &value);

	ZVAL_LONG(&value, max_live_coroutines);
	LUAEXT_SET(object, "maxLiveCoroutines", &value);
	ZVAL_LONG(&value, max_coroutine_depth);
	LUAEXT_SET(object, "maxCoroutineDepth", &value);
	ZVAL_LONG(&value, max_call_depth);
	LUAEXT_SET(object, "maxCallDepth", &value);
	ZVAL_LONG(&value, max_modules);
	LUAEXT_SET(object, "maxModules", &value);
	ZVAL_LONG(&value, max_require_depth);
	LUAEXT_SET(object, "maxRequireDepth", &value);
	ZVAL_LONG(&value, max_string_length);
	LUAEXT_SET(object, "maxStringLength", &value);
	ZVAL_LONG(&value, max_source_bytes);
	LUAEXT_SET(object, "maxSourceBytes", &value);
	ZVAL_LONG(&value, max_conversion_depth);
	LUAEXT_SET(object, "maxConversionDepth", &value);
}

LUAEXT_CONFIG_WITH_METHOD(DevelopGravity_LuaExt_Limits, luaext_ce_limits)

/* -------------------------------------------------------------------------
 * VfsQuota
 * ---------------------------------------------------------------------- */

ZEND_METHOD(DevelopGravity_LuaExt_VfsQuota, __construct)
{
	zend_long max_open_handles = LUAEXT_DEFAULT_MAX_OPEN_HANDLES;
	zend_long max_file_bytes = (zend_long)LUAEXT_DEFAULT_MAX_FILE_BYTES;
	zend_long max_total_bytes = (zend_long)LUAEXT_DEFAULT_MAX_TOTAL_BYTES;
	zend_long max_files = LUAEXT_DEFAULT_MAX_FILES;
	zend_long max_operations = LUAEXT_DEFAULT_MAX_OPERATIONS;
	zend_long max_path_length = LUAEXT_DEFAULT_MAX_PATH_LENGTH;
	zend_long max_path_depth = LUAEXT_DEFAULT_MAX_PATH_DEPTH;
	bool bill_wall_time = false;
	zend_object *object;
	zval value;

	ZEND_PARSE_PARAMETERS_START(0, 8)
	Z_PARAM_OPTIONAL
	Z_PARAM_LONG(max_open_handles)
	Z_PARAM_LONG(max_file_bytes)
	Z_PARAM_LONG(max_total_bytes)
	Z_PARAM_LONG(max_files)
	Z_PARAM_LONG(max_operations)
	Z_PARAM_LONG(max_path_length)
	Z_PARAM_LONG(max_path_depth)
	Z_PARAM_BOOL(bill_wall_time)
	ZEND_PARSE_PARAMETERS_END();

	object = Z_OBJ_P(ZEND_THIS);

	if (!luaext_config_reject_reconstruction(object)) {
		RETURN_THROWS();
	}

	ZVAL_LONG(&value, max_open_handles);
	LUAEXT_SET(object, "maxOpenHandles", &value);
	ZVAL_LONG(&value, max_file_bytes);
	LUAEXT_SET(object, "maxFileBytes", &value);
	ZVAL_LONG(&value, max_total_bytes);
	LUAEXT_SET(object, "maxTotalBytes", &value);
	ZVAL_LONG(&value, max_files);
	LUAEXT_SET(object, "maxFiles", &value);
	ZVAL_LONG(&value, max_operations);
	LUAEXT_SET(object, "maxOperations", &value);
	ZVAL_LONG(&value, max_path_length);
	LUAEXT_SET(object, "maxPathLength", &value);
	ZVAL_LONG(&value, max_path_depth);
	LUAEXT_SET(object, "maxPathDepth", &value);
	ZVAL_BOOL(&value, bill_wall_time);
	LUAEXT_SET(object, "billWallTime", &value);
}

LUAEXT_CONFIG_WITH_METHOD(DevelopGravity_LuaExt_VfsQuota, luaext_ce_vfs_quota)

/* -------------------------------------------------------------------------
 * SandboxConfig
 * ---------------------------------------------------------------------- */

/* The require() search patterns a sandbox gets when the host names none. */
static void luaext_config_default_module_paths(zval *out)
{
	zval path;

	array_init_size(out, 2);

	ZVAL_STRING(&path, "/?.lua");
	zend_hash_next_index_insert_new(Z_ARRVAL_P(out), &path);

	ZVAL_STRING(&path, "/?/init.lua");
	zend_hash_next_index_insert_new(Z_ARRVAL_P(out), &path);
}

ZEND_METHOD(DevelopGravity_LuaExt_SandboxConfig, __construct)
{
	zend_object *capabilities = NULL;
	zend_object *limits = NULL;
	zend_object *filesystem = NULL;
	zend_object *vfs_quota = NULL;
	zend_object *module_resolver = NULL;
	/* A zval, not a HashTable: see Capabilities::__construct on empty arrays. */
	zval *module_paths = NULL;
	zend_object *output_mode = NULL;
	zend_object *output_callback = NULL;
	zend_long output_chunk_bytes = LUAEXT_DEFAULT_OUTPUT_CHUNK_BYTES;
	zend_long seed = 0;
	bool seed_is_null = true;
	bool deterministic = false;
	zend_object *object;
	luaext_policy policy;
	zval value;

	ZEND_PARSE_PARAMETERS_START(0, 11)
	Z_PARAM_OPTIONAL
	Z_PARAM_OBJ_OF_CLASS_OR_NULL(capabilities, luaext_ce_capabilities)
	Z_PARAM_OBJ_OF_CLASS_OR_NULL(limits, luaext_ce_limits)
	Z_PARAM_OBJ_OF_CLASS_OR_NULL(filesystem, luaext_ce_file_system)
	Z_PARAM_OBJ_OF_CLASS_OR_NULL(vfs_quota, luaext_ce_vfs_quota)
	Z_PARAM_OBJ_OF_CLASS_OR_NULL(module_resolver, luaext_ce_module_resolver)
	Z_PARAM_ARRAY(module_paths)
	Z_PARAM_OBJ_OF_CLASS(output_mode, luaext_ce_output_mode)
	Z_PARAM_OBJ_OF_CLASS_OR_NULL(output_callback, zend_ce_closure)
	Z_PARAM_LONG(output_chunk_bytes)
	Z_PARAM_LONG_OR_NULL(seed, seed_is_null)
	Z_PARAM_BOOL(deterministic)
	ZEND_PARSE_PARAMETERS_END();

	object = Z_OBJ_P(ZEND_THIS);

	if (!luaext_config_reject_reconstruction(object)) {
		RETURN_THROWS();
	}

	/*
	 * Resolve before a single field is committed, and throw the result away:
	 * this call is here for its refusals. It reads nothing but the arguments,
	 * so a configuration that cannot be satisfied never produces an object at
	 * all — there is no half-built one to observe and none to free. The sandbox
	 * resolves the same settings again when it is built, which costs nothing
	 * because resolution is pure.
	 */
	if (!luaext_config_resolve_parts(capabilities, limits, vfs_quota, filesystem, !seed_is_null,
									 seed, deterministic, &policy)) {
		RETURN_THROWS();
	}

	if (capabilities != NULL) {
		ZVAL_OBJ_COPY(&value, capabilities);
	} else {
		ZVAL_NULL(&value);
	}
	LUAEXT_SET(object, "capabilities", &value);

	if (limits != NULL) {
		ZVAL_OBJ_COPY(&value, limits);
	} else {
		ZVAL_NULL(&value);
	}
	LUAEXT_SET(object, "limits", &value);

	if (filesystem != NULL) {
		ZVAL_OBJ_COPY(&value, filesystem);
	} else {
		ZVAL_NULL(&value);
	}
	LUAEXT_SET(object, "filesystem", &value);

	if (vfs_quota != NULL) {
		ZVAL_OBJ_COPY(&value, vfs_quota);
	} else {
		ZVAL_NULL(&value);
	}
	LUAEXT_SET(object, "vfsQuota", &value);

	if (module_resolver != NULL) {
		ZVAL_OBJ_COPY(&value, module_resolver);
	} else {
		ZVAL_NULL(&value);
	}
	LUAEXT_SET(object, "moduleResolver", &value);

	if (module_paths != NULL) {
		ZVAL_COPY(&value, module_paths);
	} else {
		luaext_config_default_module_paths(&value);
	}
	LUAEXT_SET(object, "modulePaths", &value);

	if (output_mode == NULL) {
		output_mode = zend_enum_get_case_cstr(luaext_ce_output_mode, "Buffer");
	}

	ZVAL_OBJ_COPY(&value, output_mode);
	LUAEXT_SET(object, "outputMode", &value);

	if (output_callback != NULL) {
		ZVAL_OBJ_COPY(&value, output_callback);
	} else {
		ZVAL_NULL(&value);
	}
	LUAEXT_SET(object, "outputCallback", &value);

	ZVAL_LONG(&value, output_chunk_bytes);
	LUAEXT_SET(object, "outputChunkBytes", &value);

	if (seed_is_null) {
		ZVAL_NULL(&value);
	} else {
		ZVAL_LONG(&value, seed);
	}
	LUAEXT_SET(object, "seed", &value);

	ZVAL_BOOL(&value, deterministic);
	LUAEXT_SET(object, "deterministic", &value);
}

ZEND_METHOD(DevelopGravity_LuaExt_SandboxConfig, with)
{
	zval *positional = NULL;
	uint32_t positional_count = 0;
	HashTable *named = NULL;
	luaext_policy policy;

	ZEND_PARSE_PARAMETERS_START(0, -1)
	Z_PARAM_VARIADIC_WITH_NAMED(positional, positional_count, named)
	ZEND_PARSE_PARAMETERS_END();

	(void)positional;

	if (!luaext_config_reject_positional(luaext_ce_sandbox_config, positional_count) ||
		!luaext_config_with(luaext_ce_sandbox_config, Z_OBJ_P(ZEND_THIS), named, return_value)) {
		RETURN_THROWS();
	}

	/*
	 * with() can reach a combination the source did not have — dropping the
	 * filesystem while vfs stays on, say — so the derived object is checked
	 * too. It is discarded on failure, which is the only way nothing
	 * unsatisfiable escapes this call.
	 */
	if (!luaext_config_resolve(return_value, &policy)) {
		zval_ptr_dtor(return_value);
		RETVAL_NULL();
		RETURN_THROWS();
	}
}

/* -------------------------------------------------------------------------
 * SandboxStats
 * ---------------------------------------------------------------------- */

/*
 * Field order here follows the stub, which is also the order jsonSerialize()
 * emits, so a log line reads top to bottom the way the class is documented.
 *
 * A null sandbox produces the all-zero snapshot, which is the truthful state of
 * a sandbox that has not run anything.
 */
static void luaext_config_stats_fill(zend_object *object, const luaext_sandbox *sandbox)
{
	zval value;

	if (sandbox == NULL) {
		uint32_t count = (uint32_t)object->ce->default_properties_count;
		uint32_t index;

		for (index = 0; index < count; index++) {
			const zend_property_info *info = object->ce->properties_info_table[index];
			zval *slot = OBJ_PROP_NUM(object, index);

			LUAEXT_ASSERT(Z_TYPE_P(slot) == IS_UNDEF);

			if ((ZEND_TYPE_FULL_MASK(info->type) & MAY_BE_DOUBLE) != 0) {
				ZVAL_DOUBLE(slot, 0.0);
			} else if ((ZEND_TYPE_FULL_MASK(info->type) & MAY_BE_BOOL) != 0) {
				ZVAL_FALSE(slot);
			} else {
				ZVAL_LONG(slot, 0);
			}
		}

		return;
	}

	ZVAL_LONG(&value, (zend_long)(sandbox->alloc.usage + sandbox->alloc.charged));
	LUAEXT_SET(object, "memoryBytes", &value);
	ZVAL_LONG(&value, (zend_long)sandbox->alloc.peak);
	LUAEXT_SET(object, "peakMemoryBytes", &value);
	ZVAL_LONG(&value, (zend_long)sandbox->alloc.limit);
	LUAEXT_SET(object, "memoryLimitBytes", &value);

	/*
	 * Read off the watchdog, which is the same quantity the CPU limit enforces
	 * and the same one getCpuUsage() reports -- so a host billing from these
	 * figures bills for exactly what would have stopped the script.
	 *
	 * These were hardcoded zero behind a TODO waiting for the watchdog to exist.
	 * It landed two waves ago, and the zeros stayed, which is the failure the
	 * comment itself warned about: a plausible-looking figure reaching a billing
	 * pipeline. Zero is only honest while nothing is accounted.
	 */
	ZVAL_DOUBLE(&value, luaext_timers_cpu_seconds(sandbox));
	LUAEXT_SET(object, "cpuSeconds", &value);
	ZVAL_DOUBLE(&value, luaext_timers_wall_seconds(sandbox));
	LUAEXT_SET(object, "wallClockSeconds", &value);

	ZVAL_LONG(&value, (zend_long)sandbox->out.written);
	LUAEXT_SET(object, "outputBytes", &value);
	ZVAL_BOOL(&value, sandbox->out.truncated);
	LUAEXT_SET(object, "outputTruncated", &value);
	ZVAL_LONG(&value, (zend_long)sandbox->co_live);
	LUAEXT_SET(object, "liveCoroutines", &value);
	ZVAL_LONG(&value, (zend_long)sandbox->co_peak_depth);
	LUAEXT_SET(object, "peakCoroutineDepth", &value);
	ZVAL_LONG(&value, (zend_long)sandbox->modules_loaded);
	LUAEXT_SET(object, "modulesLoaded", &value);
	ZVAL_LONG(&value, (zend_long)sandbox->vfs_operations);
	LUAEXT_SET(object, "vfsOperations", &value);
	ZVAL_LONG(&value, (zend_long)sandbox->vfs_bytes);
	LUAEXT_SET(object, "vfsBytes", &value);
	ZVAL_LONG(&value, (zend_long)sandbox->gc_collections);
	LUAEXT_SET(object, "gcCollections", &value);
	ZVAL_LONG(&value, (zend_long)sandbox->lua_calls_in);
	LUAEXT_SET(object, "luaCallsIn", &value);
	ZVAL_LONG(&value, (zend_long)sandbox->php_calls_out);
	LUAEXT_SET(object, "phpCallsOut", &value);
}

void luaext_config_stats_create(const luaext_sandbox *sandbox, zval *out)
{
	object_init_ex(out, luaext_ce_sandbox_stats);

	luaext_config_stats_fill(Z_OBJ_P(out), sandbox);
}

/*
 * Private, so a host cannot build one: a snapshot that nothing measured would
 * be indistinguishable from a real one in a log. It is a genuine constructor
 * rather than a refusal because the class has to be constructible from inside
 * the extension, and an all-zero snapshot is the truthful state of a sandbox
 * that has not run anything.
 */
ZEND_METHOD(DevelopGravity_LuaExt_SandboxStats, __construct)
{
	zend_object *object;

	ZEND_PARSE_PARAMETERS_NONE();

	object = Z_OBJ_P(ZEND_THIS);

	if (!luaext_config_reject_reconstruction(object)) {
		RETURN_THROWS();
	}

	luaext_config_stats_fill(object, NULL);
}

ZEND_METHOD(DevelopGravity_LuaExt_SandboxStats, jsonSerialize)
{
	zend_object *object;
	zend_class_entry *ce;
	uint32_t count;
	uint32_t index;

	ZEND_PARSE_PARAMETERS_NONE();

	object = Z_OBJ_P(ZEND_THIS);
	ce = object->ce;
	count = (uint32_t)ce->default_properties_count;

	array_init_size(return_value, count);

	for (index = 0; index < count; index++) {
		zval *value = OBJ_PROP_NUM(object, index);

		/* Only reachable through ReflectionClass::newInstanceWithoutConstructor(). */
		if (UNEXPECTED(Z_TYPE_P(value) == IS_UNDEF)) {
			zval_ptr_dtor(return_value);
			RETVAL_NULL();
			zend_throw_error(NULL,
							 "Typed property %s::$%s must not be accessed before initialization",
							 ZSTR_VAL(ce->name), ZSTR_VAL(ce->properties_info_table[index]->name));
			RETURN_THROWS();
		}

		Z_TRY_ADDREF_P(value);
		zend_hash_add_new(Z_ARRVAL_P(return_value), ce->properties_info_table[index]->name, value);
	}
}

/* -------------------------------------------------------------------------
 * Host integration value objects
 * ---------------------------------------------------------------------- */

ZEND_METHOD(DevelopGravity_LuaExt_FileStat, __construct)
{
	zend_long size;
	zend_long mtime;
	bool is_directory = false;
	zend_object *object;
	zval value;

	ZEND_PARSE_PARAMETERS_START(2, 3)
	Z_PARAM_LONG(size)
	Z_PARAM_LONG(mtime)
	Z_PARAM_OPTIONAL
	Z_PARAM_BOOL(is_directory)
	ZEND_PARSE_PARAMETERS_END();

	object = Z_OBJ_P(ZEND_THIS);

	if (!luaext_config_reject_reconstruction(object)) {
		RETURN_THROWS();
	}

	ZVAL_LONG(&value, size);
	LUAEXT_SET(object, "size", &value);
	ZVAL_LONG(&value, mtime);
	LUAEXT_SET(object, "mtime", &value);
	ZVAL_BOOL(&value, is_directory);
	LUAEXT_SET(object, "isDirectory", &value);
}

ZEND_METHOD(DevelopGravity_LuaExt_ModuleSource, __construct)
{
	zend_string *code;
	zend_string *chunk_name;
	bool is_bytecode = false;
	zend_object *object;
	zval value;

	ZEND_PARSE_PARAMETERS_START(2, 3)
	Z_PARAM_STR(code)
	Z_PARAM_STR(chunk_name)
	Z_PARAM_OPTIONAL
	Z_PARAM_BOOL(is_bytecode)
	ZEND_PARSE_PARAMETERS_END();

	object = Z_OBJ_P(ZEND_THIS);

	if (!luaext_config_reject_reconstruction(object)) {
		RETURN_THROWS();
	}

	ZVAL_STR_COPY(&value, code);
	LUAEXT_SET(object, "code", &value);
	ZVAL_STR_COPY(&value, chunk_name);
	LUAEXT_SET(object, "chunkName", &value);
	ZVAL_BOOL(&value, is_bytecode);
	LUAEXT_SET(object, "isBytecode", &value);
}

/*
 * The attribute instance registerObject() reads. Not a readonly value object:
 * it is constructed by the engine when an attribute is instantiated, and its
 * single property is plain data.
 */
ZEND_METHOD(DevelopGravity_LuaExt_LuaMethod, __construct)
{
	zend_string *name = NULL;
	zend_object *object;
	zval value;

	ZEND_PARSE_PARAMETERS_START(0, 1)
	Z_PARAM_OPTIONAL
	Z_PARAM_STR_OR_NULL(name)
	ZEND_PARSE_PARAMETERS_END();

	object = Z_OBJ_P(ZEND_THIS);

	if (name != NULL) {
		ZVAL_STR_COPY(&value, name);
	} else {
		ZVAL_NULL(&value);
	}

	LUAEXT_ASSIGN(object, "name", &value);
}

/* -------------------------------------------------------------------------
 * Object handlers
 * ---------------------------------------------------------------------- */

static zend_object_handlers luaext_config_handlers;

void luaext_config_startup(void)
{
	static zend_class_entry **classes[] = {
		&luaext_ce_capabilities,   &luaext_ce_limits,		 &luaext_ce_vfs_quota,
		&luaext_ce_sandbox_config, &luaext_ce_sandbox_stats, &luaext_ce_file_stat,
		&luaext_ce_module_source,
	};
	size_t index;

	memcpy(&luaext_config_handlers, &std_object_handlers, sizeof(zend_object_handlers));

	/*
	 * These objects are immutable and carry no identity, so a clone would be
	 * indistinguishable from what it was cloned from: reaching for one means
	 * either a mistake or a misunderstanding of how to derive a changed copy.
	 * with() is that path, and refusing here is what keeps it the only one.
	 */
	luaext_config_handlers.clone_obj = NULL;

	for (index = 0; index < sizeof(classes) / sizeof(classes[0]); index++) {
		(*classes[index])->default_object_handlers = &luaext_config_handlers;
	}
}
