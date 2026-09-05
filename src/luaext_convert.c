/*
 * luaext — moving values between PHP and Lua.
 *
 * The two directions fail differently, and that asymmetry is deliberate rather
 * than accidental:
 *
 *   PHP -> Lua   raises a Lua error, because it runs underneath a protected
 *                call while arguments are being pushed. lua_error() longjmps,
 *                so nothing in this direction may hold an allocation across a
 *                failure. That is why cycle detection here is a chain of
 *                pointers on the C stack rather than a hash table or a flag
 *                set on the caller's arrays: a longjmp past these frames
 *                leaves nothing behind to clean up or to un-mark.
 *
 *   Lua -> PHP   throws a PHP exception and returns false, because its callers
 *                are PHP method bodies. It never raises, so it restores the Lua
 *                stack itself on every failure path. Both entry points are
 *                bracketed with LUAEXT_NO_RAISE_BEGIN/END so a debug build
 *                asserts that, and the one allocation it makes inside the
 *                interpreter -- taking a registry slot -- runs under lua_pcall
 *                so the promise holds even out of memory.
 *
 * The push direction is deliberately NOT bracketed: raising is how it reports
 * failure, and it owns nothing that a longjmp could strand. It reads the
 * caller's zvals without taking a reference and sets no flags on them, which is
 * also what makes an immutable or interned array safe to walk here -- there is
 * no refcount to touch and nothing to un-mark on the way out.
 *
 * Tables are walked raw. Neither direction runs a metamethod — no __index, no
 * __pairs, no __len — so converting the return value of an untrusted script
 * cannot re-enter the interpreter, cannot allocate behind the memory limit's
 * back, and cannot spend unbounded time in code the conversion never chose to
 * call.
 */

#include "luaext_convert.h"

#include "luaext_alloc.h"
#include "luaext_error.h"

#include <lauxlib.h>
#include <lua.h>

#include <limits.h>
#include <stdio.h>
#include <string.h>

#include <Zend/zend_exceptions.h>

/*
 * A Lua integer is int64 in every configuration we ship, which is what lets a
 * PHP integer key survive the round trip untouched. The old extension had to
 * stringify keys past 2^53 because Lua 5.1 had only doubles; that workaround is
 * gone, and this assertion is what stops it from silently needing to come back.
 */
ZEND_STATIC_ASSERT(sizeof(lua_Integer) == 8, "luaext assumes 64-bit Lua integers");

/* -------------------------------------------------------------------------
 * Limits
 * ---------------------------------------------------------------------- */

/* Used when the policy names no depth of its own. */
#define LUAEXT_CONVERT_DEFAULT_DEPTH 64u

/*
 * Both directions recurse on the C stack, so "no limit" is not on offer: a
 * configured depth is clamped to this, and zero means the default rather than
 * unbounded.
 */
#define LUAEXT_CONVERT_DEPTH_CEILING 512u

/* Table, key, value and one slot of headroom per level of nesting. */
#define LUAEXT_CONVERT_SLOTS 4

/* Rendered path (`value[2]["name"]`) and the sentence it is appended to. */
#define LUAEXT_CONVERT_PATH_MAX 320
#define LUAEXT_CONVERT_DETAIL_MAX 320

/* Bytes of a string key shown in a message before it is elided. */
#define LUAEXT_CONVERT_KEY_MAX 32

/*
 * Ceiling on the size hint taken from a Lua table before reading it. The hint
 * comes from lua_rawlen, which a script controls.
 */
/* Elements between interrupt checks in the Lua -> PHP walk. A power of two so
 * the test is a mask; 1024 keeps the check off the per-element cost while still
 * bounding how long a walk can outlive a tripped limit. */
#define LUAEXT_CONVERT_IRQ_STRIDE 1024u

#define LUAEXT_CONVERT_SIZE_HINT_MAX 1024u

/* -------------------------------------------------------------------------
 * Registry ref slots
 * ---------------------------------------------------------------------- */

/* Slot 0 of the refs table is the freelist owner; handles start at 1. */
#define LUAEXT_REF_OWNER 0
#define LUAEXT_REF_FIRST 1

/* Ceiling on simultaneously-free slots; see luaext_convert_freelist_push(). */
#define LUAEXT_REF_FREELIST_MAX (1u << 20)

static uint32_t luaext_convert_depth_limit(const luaext_sandbox *sandbox)
{
	uint32_t configured;

	if (sandbox == NULL) {
		return LUAEXT_CONVERT_DEFAULT_DEPTH;
	}

	configured = sandbox->policy.limits.max_conversion_depth;

	if (configured == 0) {
		configured = LUAEXT_CONVERT_DEFAULT_DEPTH;
	}

	return configured < LUAEXT_CONVERT_DEPTH_CEILING ? configured : LUAEXT_CONVERT_DEPTH_CEILING;
}

/* -------------------------------------------------------------------------
 * Message building
 *
 * Messages are assembled into fixed buffers on the C stack. Nothing here
 * allocates, which is what makes it safe to call immediately before a
 * lua_error() longjmp.
 * ---------------------------------------------------------------------- */

typedef struct {
	char *data;
	size_t size;
	size_t len;
} luaext_convert_sink;

/*
 * One step of the walk: the key that led here, and the container we arrived
 * at. The chain doubles as the cycle detector — an ancestor holding the same
 * container pointer is a cycle — and as the path shown in an error message.
 */
typedef struct luaext_convert_step {
	const struct luaext_convert_step *parent;
	const void *container;

	bool has_key;

	/* How the key is written in its own language, which is what a message
	 * should echo: 3 stays 3, and "3" keeps its quotes. */
	bool key_is_string;

	/*
	 * Whether PHP stores it as an integer key. Not the same question: PHP folds
	 * the string "3" onto the integer key 3, and that fold is exactly where two
	 * distinct Lua keys become one PHP key.
	 */
	bool key_stores_as_index;

	zend_long key_index;
	const char *key_str;
	size_t key_len;
} luaext_convert_step;

/* The step a top-level value starts from: no key, no container above it. */
static void luaext_convert_step_root(luaext_convert_step *step)
{
	step->parent = NULL;
	step->container = NULL;
	step->has_key = false;
	step->key_is_string = false;
	step->key_stores_as_index = false;
	step->key_index = 0;
	step->key_str = NULL;
	step->key_len = 0;
}

static void luaext_convert_sink_init(luaext_convert_sink *sink, char *data, size_t size)
{
	sink->data = data;
	sink->size = size;
	sink->len = 0;

	if (size > 0) {
		data[0] = '\0';
	}
}

static void luaext_convert_sink_bytes(luaext_convert_sink *sink, const char *bytes, size_t count)
{
	size_t room;

	if (sink->size == 0) {
		return;
	}

	room = sink->size - 1 - sink->len;

	if (count > room) {
		count = room;
	}

	memcpy(sink->data + sink->len, bytes, count);
	sink->len += count;
	sink->data[sink->len] = '\0';
}

static void luaext_convert_sink_text(luaext_convert_sink *sink, const char *text)
{
	luaext_convert_sink_bytes(sink, text, strlen(text));
}

static void luaext_convert_sink_long(luaext_convert_sink *sink, zend_long value)
{
	char digits[32];
	int written = snprintf(digits, sizeof(digits), ZEND_LONG_FMT, value);

	if (written > 0) {
		luaext_convert_sink_bytes(sink, digits, (size_t)written);
	}
}

/*
 * Keys are binary, so a key may hold NUL bytes or arbitrary UTF-8 fragments.
 * Non-printable bytes are escaped and long keys are elided: an error message is
 * not a channel for echoing untrusted data back at whoever reads the log.
 */
static void luaext_convert_sink_key_string(luaext_convert_sink *sink, const char *key, size_t len)
{
	size_t shown = len < LUAEXT_CONVERT_KEY_MAX ? len : LUAEXT_CONVERT_KEY_MAX;
	size_t index;

	luaext_convert_sink_text(sink, "\"");

	for (index = 0; index < shown; index++) {
		unsigned char byte = (unsigned char)key[index];

		if (byte >= 0x20 && byte < 0x7f && byte != '"' && byte != '\\') {
			luaext_convert_sink_bytes(sink, (const char *)&byte, 1);
		} else {
			char escape[8];
			int written = snprintf(escape, sizeof(escape), "\\x%02X", (unsigned int)byte);

			if (written > 0) {
				luaext_convert_sink_bytes(sink, escape, (size_t)written);
			}
		}
	}

	if (shown < len) {
		luaext_convert_sink_text(sink, "...");
	}

	luaext_convert_sink_text(sink, "\"");
}

/* The key alone, as it would be written in source: 3 or "name". */
static void luaext_convert_sink_key(luaext_convert_sink *sink, const luaext_convert_step *step)
{
	if (step->key_is_string) {
		luaext_convert_sink_key_string(sink, step->key_str, step->key_len);
	} else {
		luaext_convert_sink_long(sink, step->key_index);
	}
}

/* The subscript chain from the root down to `step`, outermost key first. */
static void luaext_convert_sink_path(luaext_convert_sink *sink, const luaext_convert_step *step)
{
	if (step == NULL) {
		return;
	}

	luaext_convert_sink_path(sink, step->parent);

	if (!step->has_key) {
		return;
	}

	luaext_convert_sink_text(sink, "[");
	luaext_convert_sink_key(sink, step);
	luaext_convert_sink_text(sink, "]");
}

static void luaext_convert_render_path(const char *root, const luaext_convert_step *step,
									   char *buffer, size_t size)
{
	luaext_convert_sink sink;

	luaext_convert_sink_init(&sink, buffer, size);
	luaext_convert_sink_text(&sink, root);
	luaext_convert_sink_path(&sink, step);
}

/* -------------------------------------------------------------------------
 * PHP -> Lua
 * ---------------------------------------------------------------------- */

typedef struct {
	luaext_sandbox *sandbox;
	lua_State *L;

	/* Where the stack is rewound to before raising, so a failure leaves it
	 * exactly as the caller handed it over. */
	int base_top;

	uint32_t max_depth;
} luaext_convert_push_ctx;

static void luaext_convert_push_value(luaext_convert_push_ctx *ctx, zval *value,
									  luaext_convert_step *step, uint32_t depth);

/*
 * Abandon the conversion.
 *
 * Rewinding first matters: lua_error() unwinds to whichever protected call is
 * above us, and a half-built table left on the stack would be visible to
 * whatever runs next in this state.
 */
ZEND_COLD ZEND_NORETURN static void luaext_convert_push_fail(const luaext_convert_push_ctx *ctx,
															 const luaext_convert_step *step,
															 const char *detail)
{
	char path[LUAEXT_CONVERT_PATH_MAX];

	luaext_convert_render_path("value", step, path, sizeof(path));
	lua_settop(ctx->L, ctx->base_top);

	luaext_error_raise(ctx->L, LUAEXT_ERR_CONVERSION, true, "%s at %s", detail, path);
}

/*
 * Reserve Lua stack slots for one more level of nesting.
 *
 * luaL_checkstack() would raise its own untyped error here; growing by hand and
 * routing the failure through luaext_convert_push_fail() keeps a deep PHP array
 * arriving at the host as a ConversionError like every other refusal, rather
 * than as a bare "stack overflow" string.
 */
static void luaext_convert_push_reserve(const luaext_convert_push_ctx *ctx,
										const luaext_convert_step *step)
{
	if (!lua_checkstack(ctx->L, LUAEXT_CONVERT_SLOTS)) {
		luaext_convert_push_fail(ctx, step,
								 "Cannot convert a PHP array to Lua: the interpreter stack cannot "
								 "grow far enough to hold it");
	}
}

static void luaext_convert_push_array(luaext_convert_push_ctx *ctx, zend_array *array,
									  luaext_convert_step *step, uint32_t depth)
{
	lua_State *L = ctx->L;
	const luaext_convert_step *ancestor;
	uint32_t count;
	zend_ulong numeric_key;
	zend_string *string_key;
	zval *element;

	if (depth >= ctx->max_depth) {
		char detail[LUAEXT_CONVERT_DETAIL_MAX];

		snprintf(detail, sizeof(detail),
				 "Cannot convert a PHP array nested deeper than %u levels to Lua", ctx->max_depth);
		luaext_convert_push_fail(ctx, step, detail);
	}

	/*
	 * Cycle detection walks the ancestors rather than every array seen so far:
	 * the same array appearing twice as a sibling is a shared subtree, which
	 * converts perfectly well into two Lua tables. Only an array that contains
	 * itself has no Lua representation.
	 */
	for (ancestor = step->parent; ancestor != NULL; ancestor = ancestor->parent) {
		if (ancestor->container == array) {
			luaext_convert_push_fail(ctx, step,
									 "Cannot convert a circular PHP array to Lua: this element is "
									 "one of its own parents");
		}
	}

	step->container = array;

	luaext_convert_push_reserve(ctx, step);

	count = zend_hash_num_elements(array);

	if (count > (uint32_t)INT_MAX) {
		count = (uint32_t)INT_MAX;
	}

	/*
	 * A list becomes a Lua sequence, so it is worth telling the interpreter to
	 * size the array part rather than the hash part.
	 */
	if (zend_array_is_list(array)) {
		lua_createtable(L, (int)count, 0);
	} else {
		lua_createtable(L, 0, (int)count);
	}

	ZEND_HASH_FOREACH_KEY_VAL(array, numeric_key, string_key, element)
	{
		luaext_convert_step child;

		child.parent = step;
		child.container = NULL;
		child.has_key = true;

		if (string_key != NULL) {
			child.key_is_string = true;
			child.key_stores_as_index = false;
			child.key_index = 0;
			child.key_str = ZSTR_VAL(string_key);
			child.key_len = ZSTR_LEN(string_key);

			lua_pushlstring(L, ZSTR_VAL(string_key), ZSTR_LEN(string_key));
		} else {
			child.key_is_string = false;
			child.key_stores_as_index = true;
			child.key_index = (zend_long)numeric_key;
			child.key_str = NULL;
			child.key_len = 0;

			/*
			 * The full int64 range, pushed as an integer. PHP and Lua agree on
			 * integer keys now, so nothing above 2^53 needs stringifying and
			 * PHP_INT_MIN survives as itself.
			 */
			lua_pushinteger(L, (lua_Integer)(zend_long)numeric_key);
		}

		luaext_convert_push_value(ctx, element, &child, depth + 1);

		/* Raw: the table was created here and has no metatable, so a metamethod
		 * could only come from somewhere it has no business coming from. */
		lua_rawset(L, -3);
	}
	ZEND_HASH_FOREACH_END();
}

static void luaext_convert_push_object(luaext_convert_push_ctx *ctx, zval *value,
									   const luaext_convert_step *step)
{
	zend_object *object = Z_OBJ_P(value);
	char detail[LUAEXT_CONVERT_DETAIL_MAX];

	if (!instanceof_function(object->ce, luaext_ce_lua_function)) {
		/*
		 * Deliberately not "convertible with a bit more work": an object that
		 * crossed into Lua would have to carry identity and behaviour with it,
		 * and registerObject() is the one bridge that exposes behaviour without
		 * exposing the object.
		 */
		snprintf(detail, sizeof(detail),
				 "Cannot convert an instance of %s to Lua; only LuaFunction values from this "
				 "sandbox have a Lua representation",
				 ZSTR_VAL(object->ce->name));
		luaext_convert_push_fail(ctx, step, detail);
	}

	{
		luaext_function_obj *function = luaext_function_from_obj(object);

		/*
		 * A function handle names a slot in one sandbox's registry. Pushing it
		 * into a different interpreter would either read an unrelated slot or
		 * hand one state a value owned by another; both are worse than a
		 * refusal.
		 */
		if (ctx->sandbox == NULL || Z_TYPE(function->sandbox_zv) != IS_OBJECT ||
			Z_OBJ(function->sandbox_zv) != &ctx->sandbox->std) {
			luaext_convert_push_fail(
				ctx, step, "Cannot convert a LuaFunction belonging to a different sandbox to Lua");
		}

		if (function->ref < LUAEXT_REF_FIRST) {
			luaext_convert_push_fail(
				ctx, step, "Cannot convert a LuaFunction that no longer references a Lua function");
		}

		luaext_convert_push_reserve(ctx, step);
		luaext_convert_ref_push(ctx->sandbox, ctx->L, function->ref);

		if (!lua_isfunction(ctx->L, -1)) {
			lua_pop(ctx->L, 1);
			luaext_convert_push_fail(
				ctx, step, "Cannot convert a LuaFunction whose registry slot has been released");
		}
	}
}

static void luaext_convert_push_value(luaext_convert_push_ctx *ctx, zval *value,
									  luaext_convert_step *step, uint32_t depth)
{
	lua_State *L = ctx->L;

	/*
	 * A reference is transparent; what matters is what it points at. That also
	 * covers `$array[] = &$array`, which is the only way a PHP array can be
	 * cyclic — the dereferenced array is the same zend_array an ancestor step
	 * already recorded.
	 */
	ZVAL_DEREF(value);

	switch (Z_TYPE_P(value)) {
	case IS_UNDEF:
	case IS_NULL:
		lua_pushnil(L);
		return;

	case IS_FALSE:
		lua_pushboolean(L, 0);
		return;

	case IS_TRUE:
		lua_pushboolean(L, 1);
		return;

	case IS_LONG:
		lua_pushinteger(L, (lua_Integer)Z_LVAL_P(value));
		return;

	case IS_DOUBLE:
		lua_pushnumber(L, (lua_Number)Z_DVAL_P(value));
		return;

	case IS_STRING:
		/* Explicit length throughout: PHP strings are binary and a NUL byte in
		 * the middle of one is data, not a terminator. */
		lua_pushlstring(L, Z_STRVAL_P(value), Z_STRLEN_P(value));
		return;

	case IS_ARRAY:
		luaext_convert_push_array(ctx, Z_ARRVAL_P(value), step, depth);
		return;

	case IS_OBJECT:
		luaext_convert_push_object(ctx, value, step);
		return;

	default: {
		char detail[LUAEXT_CONVERT_DETAIL_MAX];

		snprintf(detail, sizeof(detail), "Cannot convert a PHP %s to Lua",
				 zend_zval_type_name(value));
		luaext_convert_push_fail(ctx, step, detail);
	}
	}
}

void luaext_convert_push_zval(luaext_sandbox *sandbox, lua_State *L, zval *value)
{
	luaext_convert_push_ctx ctx;
	luaext_convert_step root;

	ctx.sandbox = sandbox;
	ctx.L = L;
	ctx.base_top = lua_gettop(L);
	ctx.max_depth = luaext_convert_depth_limit(sandbox);

	luaext_convert_step_root(&root);

	luaext_convert_push_value(&ctx, value, &root, 0);
}

/* -------------------------------------------------------------------------
 * Lua -> PHP
 * ---------------------------------------------------------------------- */

typedef struct {
	luaext_sandbox *sandbox;
	lua_State *L;

	/* What the rendered path starts with: "value", "return value 2", ... */
	const char *root;

	uint32_t max_depth;

	/*
	 * Whether the PHP-side bytes this conversion allocates are charged against
	 * the sandbox's memory limit -- and why that is the caller's decision rather
	 * than a property of converting.
	 *
	 * A Lua string is already billed by lua_Alloc, but the PHP copy made here is
	 * a SECOND allocation the limit never saw: hand a callback a large string and
	 * the process holds both, so a sandbox capped at N bytes transiently holds
	 * appreciably more. Tables are worse than strings, since a PHP array costs
	 * more per element than a Lua table does.
	 *
	 * It cannot simply be charged everywhere, because a charge is only honest if
	 * something discharges it. Of the three callers, exactly one owns the zvals
	 * it asks for: the callback bridge frees its params array when the call
	 * returns. The other two -- an eval/call result and a global getter -- hand
	 * the zval to PHP, whose lifetime we neither know nor control, so charging
	 * there would burn budget that is never given back and shrink the effective
	 * limit on every call until the sandbox refused to run.
	 *
	 * So the converter MEASURES (always, into `produced`) and the boundary that
	 * owns the lifetime DECIDES (by setting `bill`). Callers that bill must
	 * discharge `produced` when they release the values.
	 */
	bool bill;
	size_t produced;

	/* Elements walked, for the strided interrupt check. Counts across the whole
	 * conversion rather than per table, so a wide tree of small tables is bounded
	 * the same way one large table is. */
	uint32_t elements;
} luaext_convert_to_ctx;

static bool luaext_convert_value(luaext_convert_to_ctx *ctx, int index, luaext_convert_step *step,
								 uint32_t depth, zval *out);

/*
 * Account for `bytes` of PHP-side allocation, refusing when it does not fit.
 *
 * Charged BEFORE the allocation it describes, so a value too large for the
 * remaining budget is refused instead of being built and then complained about.
 * Always accumulates into `produced` even when not billing, so a caller can see
 * what a conversion cost without having paid for it.
 */
static bool luaext_convert_account(luaext_convert_to_ctx *ctx, size_t bytes,
								   const luaext_convert_step *step)
{
	ctx->produced += bytes;

	if (!ctx->bill || bytes == 0) {
		return true;
	}

	if (!luaext_alloc_charge(ctx->sandbox, bytes)) {
		/*
		 * MemoryLimitError, not ConversionError: the value is perfectly
		 * convertible and the sandbox simply cannot afford it. Reporting it as a
		 * conversion problem would send a host looking for a type bug.
		 */
		char path[LUAEXT_CONVERT_PATH_MAX];

		luaext_convert_render_path(ctx->root, step, path, sizeof(path));
		zend_throw_exception_ex(luaext_ce_memory_limit_error, 0,
								"Converting %s for the host would need %zu more byte(s) than the "
								"sandbox's memory limit allows",
								path, bytes);
		return false;
	}

	return true;
}

ZEND_COLD static bool luaext_convert_to_fail(const luaext_convert_to_ctx *ctx,
											 const luaext_convert_step *step, const char *detail)
{
	char path[LUAEXT_CONVERT_PATH_MAX];

	luaext_convert_render_path(ctx->root, step, path, sizeof(path));
	zend_throw_exception_ex(luaext_ce_conversion_error, 0, "%s at %s", detail, path);

	return false;
}

static void luaext_convert_number(lua_State *L, int index, zval *out)
{
	/*
	 * lua_isinteger() rather than a floor() test: since Lua gained a real
	 * integer type, 2 and 2.0 are different values, and collapsing them into
	 * one PHP int would lose a distinction the script deliberately made.
	 */
	if (lua_isinteger(L, index)) {
		lua_Integer value = lua_tointeger(L, index);

#if SIZEOF_ZEND_LONG >= 8
		ZVAL_LONG(out, (zend_long)value);
#else
		/* A 32-bit PHP cannot hold every Lua integer; the ones it cannot hold
		 * become floats, which is what PHP itself does with such literals. */
		if (value < (lua_Integer)ZEND_LONG_MIN || value > (lua_Integer)ZEND_LONG_MAX) {
			ZVAL_DOUBLE(out, (double)value);
		} else {
			ZVAL_LONG(out, (zend_long)value);
		}
#endif
		return;
	}

	ZVAL_DOUBLE(out, (double)lua_tonumber(L, index));
}

static bool luaext_convert_function(luaext_convert_to_ctx *ctx, int index,
									const luaext_convert_step *step, zval *out)
{
	luaext_function_obj *function;
	int ref;

	if (ctx->sandbox == NULL) {
		return luaext_convert_to_fail(ctx, step,
									  "Cannot convert a Lua function without an owning sandbox");
	}

	/*
	 * The registry slot is taken before the handle exists. Reserving it can
	 * raise a Lua memory error, and doing that while holding a live zval would
	 * longjmp past its release.
	 */
	ref = luaext_convert_ref_create(ctx->sandbox, ctx->L, index);

	if (ref < LUAEXT_REF_FIRST) {
		return false;
	}

	object_init_ex(out, luaext_ce_lua_function);

	function = Z_LUAEXT_FUNCTION_P(out);
	function->ref = ref;
	ZVAL_OBJ_COPY(&function->sandbox_zv, &ctx->sandbox->std);

	return true;
}

/*
 * Work out the PHP key a Lua key becomes, and refuse the ones that have no
 * faithful answer.
 *
 * PHP normalises "1" to the integer key 1, so a Lua table holding both t[1] and
 * t["1"] describes two values PHP can only store as one. Detecting that here,
 * by key rather than after the fact, is what turns silent data loss into a
 * refusal that names the key responsible.
 */
static bool luaext_convert_key(luaext_convert_to_ctx *ctx, int key_index,
							   const luaext_convert_step *step, luaext_convert_step *child)
{
	lua_State *L = ctx->L;
	int type = lua_type(L, key_index);

	child->has_key = true;

	if (type == LUA_TNUMBER) {
		lua_Integer value;

		if (!lua_isinteger(L, key_index)) {
			char detail[LUAEXT_CONVERT_DETAIL_MAX];

			/*
			 * Lua itself normalises a float key with an exact integer value to
			 * an integer key, so anything still floating here is genuinely
			 * fractional or out of integer range. PHP would truncate it and
			 * merge it with a neighbour.
			 */
			snprintf(detail, sizeof(detail),
					 "Cannot convert a Lua table to a PHP array: the key %.14g is a float and PHP "
					 "array keys are integers or strings",
					 (double)lua_tonumber(L, key_index));
			return luaext_convert_to_fail(ctx, step, detail);
		}

		value = lua_tointeger(L, key_index);

#if SIZEOF_ZEND_LONG < 8
		if (value < (lua_Integer)ZEND_LONG_MIN || value > (lua_Integer)ZEND_LONG_MAX) {
			return luaext_convert_to_fail(ctx, step,
										  "Cannot convert a Lua table to a PHP array: a key lies "
										  "outside PHP's integer range");
		}
#endif

		child->key_is_string = false;
		child->key_stores_as_index = true;
		child->key_index = (zend_long)value;
		child->key_str = NULL;
		child->key_len = 0;

		return true;
	}

	if (type == LUA_TSTRING) {
		zend_ulong numeric;
		size_t length;

		/* Safe on a value that is already a string: lua_tolstring only rewrites
		 * the stack slot when it has to convert a number. */
		child->key_str = lua_tolstring(L, key_index, &length);
		child->key_len = length;
		child->key_is_string = true;
		child->key_index = 0;

		/* Exactly PHP's own rule, applied to the raw bytes so an embedded NUL
		 * cannot make a key look numeric when PHP would not agree. */
		child->key_stores_as_index =
			ZEND_HANDLE_NUMERIC_STR(child->key_str, child->key_len, numeric) != 0;

		if (child->key_stores_as_index) {
			child->key_index = (zend_long)numeric;
		}

		return true;
	}

	{
		char detail[LUAEXT_CONVERT_DETAIL_MAX];

		snprintf(detail, sizeof(detail),
				 "Cannot convert a Lua table to a PHP array: a %s cannot be a PHP array key",
				 lua_typename(L, type));
		return luaext_convert_to_fail(ctx, step, detail);
	}
}

static bool luaext_convert_key_collides(const luaext_convert_step *child, const HashTable *target)
{
	if (child->key_stores_as_index) {
		return zend_hash_index_exists(target, (zend_ulong)child->key_index);
	}

	return zend_hash_str_exists(target, child->key_str, child->key_len);
}

/* How PHP would write the key, which is the half of the collision the caller
 * cannot see from the Lua side. */
static void luaext_convert_sink_php_key(luaext_convert_sink *sink, const luaext_convert_step *child)
{
	if (child->key_stores_as_index) {
		luaext_convert_sink_long(sink, child->key_index);
	} else {
		luaext_convert_sink_key_string(sink, child->key_str, child->key_len);
	}
}

static bool luaext_convert_table(luaext_convert_to_ctx *ctx, int index, luaext_convert_step *step,
								 uint32_t depth, zval *out)
{
	lua_State *L = ctx->L;
	const luaext_convert_step *ancestor;
	const void *identity = lua_topointer(L, index);
	HashTable *target;
	int top = lua_gettop(L);

	if (depth >= ctx->max_depth) {
		char detail[LUAEXT_CONVERT_DETAIL_MAX];

		snprintf(detail, sizeof(detail),
				 "Cannot convert a Lua table nested deeper than %u levels to PHP", ctx->max_depth);
		return luaext_convert_to_fail(ctx, step, detail);
	}

	/* Ancestors only, for the same reason as the other direction: a table
	 * reachable twice is a shared subtree, not a cycle. */
	for (ancestor = step->parent; ancestor != NULL; ancestor = ancestor->parent) {
		if (ancestor->container == identity) {
			return luaext_convert_to_fail(
				ctx, step,
				"Cannot convert a circular Lua table to PHP: this table is one of its own parents");
		}
	}

	step->container = identity;

	if (!lua_checkstack(L, LUAEXT_CONVERT_SLOTS + 1)) {
		return luaext_convert_to_fail(
			ctx, step,
			"Cannot convert a Lua table to PHP: the interpreter stack cannot grow far enough");
	}

	/*
	 * Only a hint, and a clamped one. lua_rawlen returns a border, which a
	 * script can push arbitrarily high with a single sparse assignment; sizing
	 * a PHP array to an attacker-chosen number before a single element has been
	 * read would be a one-line memory exhaustion.
	 */
	{
		lua_Unsigned sequence = lua_rawlen(L, index);

		array_init_size(out, sequence < LUAEXT_CONVERT_SIZE_HINT_MAX
								 ? (uint32_t)sequence
								 : LUAEXT_CONVERT_SIZE_HINT_MAX);
	}

	target = Z_ARRVAL_P(out);

	lua_pushnil(L);

	while (lua_next(L, index) != 0) {
		luaext_convert_step child;
		zval element;
		int value_index = lua_gettop(L);

		/*
		 * Asked, not raised. LUAEXT_CHECK would longjmp, and this loop holds a
		 * half-built PHP array plus two Lua stack slots -- so it unwinds through
		 * the ordinary failure path instead, which releases both.
		 *
		 * Strided, like the interrupt checks patched into the vendored string and
		 * utf8 loops, because this one is per ELEMENT rather than per allocation:
		 * a relaxed load is cheap but a million of them is not free, and the
		 * quantity being bounded is time, which a stride still bounds.
		 *
		 * WHAT THIS CATCHES, precisely. The callback bridge already tests the flag
		 * before entering PHP, so a conversion that starts after a breach never
		 * begins. This is the other half: a conversion ALREADY UNDERWAY when the
		 * watchdog raises the flag mid-walk. Before it, that walk ran to
		 * completion however long it took -- bounded by memoryBytes, since the
		 * table had to be built inside the sandbox, but not by the CPU or
		 * wall-clock limit the host actually set.
		 *
		 * That window is a race with a background thread and has no deterministic
		 * test; it is guarded by reasoning rather than by a .phpt, which is worth
		 * stating plainly rather than implying coverage that does not exist.
		 */
		if ((++ctx->elements & (LUAEXT_CONVERT_IRQ_STRIDE - 1u)) == 0u &&
			luaext_interrupt_pending(L)) {
			(void)luaext_convert_to_fail(
				ctx, step, "Converting a Lua table to PHP was interrupted by a limit");
			goto failed;
		}

		child.parent = step;
		child.container = NULL;

		/* Key first, and refused before any work is done on the value it
		 * carries: a collision is a property of the key alone. */
		if (!luaext_convert_key(ctx, value_index - 1, step, &child)) {
			goto failed;
		}

		if (luaext_convert_key_collides(&child, target)) {
			char detail[LUAEXT_CONVERT_DETAIL_MAX];
			luaext_convert_sink sink;

			luaext_convert_sink_init(&sink, detail, sizeof(detail));
			luaext_convert_sink_text(&sink, "Cannot convert a Lua table to a PHP array: the key ");
			luaext_convert_sink_key(&sink, &child);
			luaext_convert_sink_text(&sink, " collides with a key the table already carries, "
											"because PHP stores both of them as ");
			luaext_convert_sink_php_key(&sink, &child);

			(void)luaext_convert_to_fail(ctx, step, detail);
			goto failed;
		}

		ZVAL_UNDEF(&element);

		if (!luaext_convert_value(ctx, value_index, &child, depth + 1, &element)) {
			goto failed;
		}

		/* Drop the value; lua_next needs the key left where it is. child.key_str
		 * still points into that key, which keeps it anchored against the GC. */
		lua_pop(L, 1);

		/*
		 * One hash bucket per entry, plus the key when it is a string. Charged
		 * per element rather than up front because the array was sized from a
		 * hint that a script can inflate with one sparse assignment (see
		 * array_init_size above) -- billing that hint would let a table claiming
		 * a million entries charge for a million it does not have.
		 */
		if (!luaext_convert_account(
				ctx, sizeof(Bucket) + (child.key_stores_as_index ? 0 : child.key_len), step)) {
			zval_ptr_dtor(&element);
			goto failed;
		}

		if (child.key_stores_as_index) {
			zend_hash_index_update(target, (zend_ulong)child.key_index, &element);
		} else {
			zend_hash_str_update(target, child.key_str, child.key_len, &element);
		}
	}

	return true;

failed:
	lua_settop(L, top);
	zval_ptr_dtor(out);
	ZVAL_NULL(out);

	return false;
}

static bool luaext_convert_value(luaext_convert_to_ctx *ctx, int index, luaext_convert_step *step,
								 uint32_t depth, zval *out)
{
	lua_State *L = ctx->L;

	/* Something valid before anything can fail: callers hand over an
	 * uninitialised zval and must be able to release it either way. */
	ZVAL_NULL(out);

	switch (lua_type(L, index)) {
	case LUA_TNIL:
		return true;

	case LUA_TBOOLEAN:
		ZVAL_BOOL(out, lua_toboolean(L, index));
		return true;

	case LUA_TNUMBER:
		luaext_convert_number(L, index, out);
		return true;

	case LUA_TSTRING: {
		size_t length;
		const char *bytes = lua_tolstring(L, index, &length);

		if (!luaext_convert_account(ctx, length + sizeof(zend_string), step)) {
			return false;
		}

		/* Length-carrying copy: a Lua string is a byte string and its NUL
		 * bytes are content. */
		ZVAL_STRINGL_FAST(out, bytes, length);
		return true;
	}

	case LUA_TTABLE:
		return luaext_convert_table(ctx, index, step, depth, out);

	case LUA_TFUNCTION:
		return luaext_convert_function(ctx, index, step, out);

	case LUA_TTHREAD:
		return luaext_convert_to_fail(ctx, step,
									  "Cannot convert a Lua coroutine to PHP: coroutines are an "
									  "in-script tool and have no PHP-side handle");

	case LUA_TUSERDATA:
	case LUA_TLIGHTUSERDATA:
		return luaext_convert_to_fail(
			ctx, step, "Cannot convert Lua userdata to PHP: it would hand out a raw pointer");

	case LUA_TNONE:
		return luaext_convert_to_fail(ctx, step, "There is no Lua value at that stack position");

	default:
		return luaext_convert_to_fail(ctx, step, "Cannot convert this Lua value to PHP");
	}
}

static bool luaext_convert_to_zval_inner(luaext_sandbox *sandbox, lua_State *L, int index,
										 zval *out, bool bill, size_t *billed)
{
	/* Zeroed rather than assigned field by field: `bill` and `produced` decide
	 * whether the memory limit is charged, so a field left out is not a missing
	 * value but a wrong one. */
	luaext_convert_to_ctx ctx = {0};
	luaext_convert_step root;
	bool converted;

	ctx.sandbox = sandbox;
	ctx.L = L;
	ctx.root = "value";
	ctx.max_depth = luaext_convert_depth_limit(sandbox);
	ctx.bill = bill;
	ctx.produced = 0;
	ctx.elements = 0;

	luaext_convert_step_root(&root);

	/*
	 * A zval is owned from here down -- `out`, and the part-built arrays inside
	 * it -- so raising would abandon it. Everything below throws and returns
	 * instead; this arms the debug assertion that says so.
	 */
	LUAEXT_NO_RAISE_BEGIN(L);
	converted = luaext_convert_value(&ctx, lua_absindex(L, index), &root, 0, out);
	LUAEXT_NO_RAISE_END(L);

	/* Reported even on failure: a partial conversion has already charged for
	 * what it managed to build, and the caller has to give that back. */
	if (billed != NULL) {
		*billed = ctx.produced;
	}

	return converted;
}

bool luaext_convert_to_zval(luaext_sandbox *sandbox, lua_State *L, int index, zval *out)
{
	return luaext_convert_to_zval_inner(sandbox, L, index, out, false, NULL);
}

bool luaext_convert_to_zval_billed(luaext_sandbox *sandbox, lua_State *L, int index, zval *out,
								   size_t *billed)
{
	return luaext_convert_to_zval_inner(sandbox, L, index, out, true, billed);
}

bool luaext_convert_stack_to_array(luaext_sandbox *sandbox, lua_State *L, int first, int count,
								   zval *out)
{
	int top = lua_gettop(L);
	int base;
	int offset;
	bool converted = true;

	if (count <= 0) {
		array_init(out);
		return true;
	}

	base = lua_absindex(L, first);

	/* Checked before the array exists, so this path owns nothing to unwind. */
	if (base < 1 || base > top || base > top - count + 1) {
		array_init(out);
		zend_throw_exception_ex(luaext_ce_conversion_error, 0,
								"Cannot read %d value(s) from the Lua stack: only %d are present",
								count, top - base + 1 > 0 ? top - base + 1 : 0);

		return false;
	}

	array_init_size(out, (uint32_t)count);

	LUAEXT_NO_RAISE_BEGIN(L);

	for (offset = 0; offset < count && converted; offset++) {
		/*
		 * Zeroed, and that is a FIX rather than tidying: this site assigned only
		 * sandbox/L/root/max_depth and left `bill` and `produced` as stack
		 * garbage, both of which luaext_convert_account() reads. A non-zero
		 * `bill` here charges the memory limit for values on the results path --
		 * the one path the design says must never be charged, because nothing
		 * ever discharges it. It would have shrunk the effective limit on every
		 * call until the sandbox refused to run, and only stayed invisible
		 * because that stack slot happened to be zero.
		 */
		luaext_convert_to_ctx ctx = {0};
		luaext_convert_step root;
		char label[48];
		zval element;

		snprintf(label, sizeof(label), "return value %d", offset + 1);

		ctx.sandbox = sandbox;
		ctx.L = L;
		ctx.root = label;
		ctx.max_depth = luaext_convert_depth_limit(sandbox);

		luaext_convert_step_root(&root);

		ZVAL_UNDEF(&element);

		converted = luaext_convert_value(&ctx, base + offset, &root, 0, &element);

		if (converted) {
			/* Zero-indexed and in stack order, which is the shape every
			 * multi-return arrives in on the PHP side. */
			add_next_index_zval(out, &element);
		}
	}

	LUAEXT_NO_RAISE_END(L);

	if (!converted) {
		zval_ptr_dtor(out);
		array_init(out);
	}

	return converted;
}

/* -------------------------------------------------------------------------
 * Registry ref slots
 *
 * One table in the registry, keyed by the address of luaext_key_refs, mapping
 * small integers to Lua values so a PHP handle can name one.
 *
 * Released slots go on a freelist and are handed out again. The extension this
 * replaces only ever incremented its counter, so a worker sandbox that compiled
 * and dropped functions for long enough walked it toward INT_MAX and grew a
 * registry table full of holes on the way. Reuse bounds both by the number of
 * handles alive at once instead of the number ever created.
 * ---------------------------------------------------------------------- */

/*
 * Owns the freelist array on Lua's behalf.
 *
 * The array is plain process memory rather than request memory, matching the
 * Lua heap: a sandbox may outlive the request that built it in a worker SAPI.
 * That leaves the question of who frees it, and the answer is lua_close(),
 * which runs this finaliser — so the freelist cannot outlive the state it
 * describes and needs no cooperation from the sandbox lifecycle.
 */
typedef struct {
	luaext_sandbox *sandbox;
} luaext_ref_owner;

static int luaext_convert_refs_release_owner(lua_State *L)
{
	luaext_ref_owner *owner = (luaext_ref_owner *)lua_touserdata(L, 1);

	if (owner == NULL || owner->sandbox == NULL) {
		return 0;
	}

	if (owner->sandbox->ref_freelist != NULL) {
		pefree(owner->sandbox->ref_freelist, 1);
		owner->sandbox->ref_freelist = NULL;
	}

	owner->sandbox->ref_freelist_len = 0;
	owner->sandbox->ref_freelist_cap = 0;
	owner->sandbox = NULL;

	return 0;
}

/* Pushes the refs table. Returns false, having pushed nothing, when there is
 * none and `create` is false. */
static bool luaext_convert_refs_table(luaext_sandbox *sandbox, lua_State *L, bool create)
{
	if (lua_rawgetp(L, LUA_REGISTRYINDEX, &luaext_key_refs) == LUA_TTABLE) {
		return true;
	}

	lua_pop(L, 1);

	if (!create) {
		return false;
	}

	lua_createtable(L, 8, 1);

	/*
	 * Slot 0 is the finaliser that owns the freelist. Setting a metatable that
	 * already carries __gc is what marks the userdata for finalisation, so the
	 * order here is load bearing.
	 */
	{
		luaext_ref_owner *owner = (luaext_ref_owner *)lua_newuserdatauv(L, sizeof(*owner), 0);

		owner->sandbox = sandbox;

		lua_createtable(L, 0, 2);
		lua_pushcfunction(L, luaext_convert_refs_release_owner);
		lua_setfield(L, -2, "__gc");
		lua_pushboolean(L, 0);
		lua_setfield(L, -2, "__metatable");
		lua_setmetatable(L, -2);

		lua_rawseti(L, -2, LUAEXT_REF_OWNER);
	}

	lua_pushvalue(L, -1);
	lua_rawsetp(L, LUA_REGISTRYINDEX, &luaext_key_refs);

	return true;
}

static bool luaext_convert_freelist_push(luaext_sandbox *sandbox, int ref)
{
	if (sandbox->ref_freelist_len == sandbox->ref_freelist_cap) {
		uint32_t grown = sandbox->ref_freelist_cap != 0 ? sandbox->ref_freelist_cap * 2 : 8;

		/*
		 * The freelist can only be as long as the number of slots alive at
		 * once, so this ceiling is unreachable short of a million simultaneous
		 * handles. Past it a released slot is simply not recycled, which costs
		 * an index rather than correctness.
		 */
		if (grown > LUAEXT_REF_FREELIST_MAX) {
			return false;
		}

		sandbox->ref_freelist =
			(int *)safe_perealloc(sandbox->ref_freelist, grown, sizeof(int), 0, 1);
		sandbox->ref_freelist_cap = grown;
	}

	sandbox->ref_freelist[sandbox->ref_freelist_len++] = ref;

	return true;
}

/*
 * The allocating half of taking a slot, run under lua_pcall.
 *
 * Creating the refs table and storing into it are the only allocations the
 * Lua->PHP direction makes inside the interpreter, and an allocation failure in
 * Lua is a longjmp. Every caller here is a PHP method body part-way through
 * building a zval, so a longjmp would abandon it. Protecting these two calls is
 * what makes "throws a PHP exception and returns" true rather than
 * true-unless-out-of-memory.
 *
 * Arguments: 1 = the value to reference, 2 = the slot to store it in.
 */
static int luaext_convert_ref_store(lua_State *L)
{
	(void)luaext_convert_refs_table(LUAEXT_SB(L), L, true);

	lua_pushvalue(L, 1);
	lua_rawseti(L, -2, lua_tointeger(L, 2));

	return 0;
}

int luaext_convert_ref_create(luaext_sandbox *sandbox, lua_State *L, int index)
{
	int ref;

	if (sandbox == NULL || L == NULL) {
		zend_throw_exception(luaext_ce_conversion_error,
							 "Cannot reference a Lua value without an open sandbox", 0);
		return -1;
	}

	index = lua_absindex(L, index);

	if (!lua_checkstack(L, 5)) {
		zend_throw_exception(luaext_ce_conversion_error,
							 "Cannot reference a Lua value: the interpreter stack cannot grow", 0);
		return -1;
	}

	if (sandbox->ref_freelist_len > 0) {
		ref = sandbox->ref_freelist[--sandbox->ref_freelist_len];
	} else {
		if (sandbox->next_ref < LUAEXT_REF_FIRST) {
			sandbox->next_ref = LUAEXT_REF_FIRST;
		}

		if (sandbox->next_ref == INT_MAX) {
			zend_throw_exception(luaext_ce_conversion_error,
								 "Cannot reference a Lua value: the sandbox has no free registry "
								 "slots left",
								 0);
			return -1;
		}

		ref = sandbox->next_ref++;
	}

	lua_pushcfunction(L, luaext_convert_ref_store);
	lua_pushvalue(L, index);
	lua_pushinteger(L, (lua_Integer)ref);

	if (lua_pcall(L, 2, 0, 0) != LUA_OK) {
		const char *message = lua_tostring(L, -1);

		/* Nothing was written, so the slot goes straight back rather than being
		 * stranded between the counter and the freelist. */
		(void)luaext_convert_freelist_push(sandbox, ref);

		zend_throw_exception_ex(luaext_ce_conversion_error, 0, "Cannot reference a Lua value: %s",
								message != NULL ? message : "the interpreter refused the store");
		lua_pop(L, 1);

		return -1;
	}

	return ref;
}

void luaext_convert_ref_push(luaext_sandbox *sandbox, lua_State *L, int ref)
{
	if (L == NULL) {
		return;
	}

	if (sandbox == NULL || ref < LUAEXT_REF_FIRST ||
		!luaext_convert_refs_table(sandbox, L, false)) {
		lua_pushnil(L);
		return;
	}

	lua_rawgeti(L, -1, (lua_Integer)ref);
	lua_remove(L, -2);
}

void luaext_convert_ref_release(luaext_sandbox *sandbox, lua_State *L, int ref)
{
	if (sandbox == NULL || L == NULL || ref < LUAEXT_REF_FIRST) {
		return;
	}

	/*
	 * Clearing the slot before recycling it matters: the value it held must
	 * become collectable now rather than when the index is next handed out.
	 */
	if (lua_checkstack(L, 2) && luaext_convert_refs_table(sandbox, L, false)) {
		lua_pushnil(L);
		lua_rawseti(L, -2, (lua_Integer)ref);
		lua_pop(L, 1);
	}

	(void)luaext_convert_freelist_push(sandbox, ref);
}
