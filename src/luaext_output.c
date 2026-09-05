/*
 * luaext — where a script's output goes.
 *
 * A sandbox has no stdout. print, io.write and warn all end here, and the host
 * chooses between buffering, a streaming callback, or discarding.
 *
 * Four properties this file owns, in rough order of how much they matter:
 *
 *   The budget. Limits::$outputBytes caps what a script may emit, and
 *   OverflowBehavior decides what happens at the cap. The decision lives here
 *   rather than in print, because this is what knows the behaviour: Truncate
 *   records the loss and reports success, Fail reports failure and lets the
 *   caller raise a FATAL OutputLimitError. A script must not be able to pcall
 *   its way past its own output budget.
 *
 *   The billing. The buffer is host memory that lua_Alloc never sees, so every
 *   byte of it is charged against memoryBytes through luaext_alloc_charge().
 *   Without that, a script that cannot allocate a large Lua string can still
 *   exhaust the same budget by printing one. The buffer's capacity is managed
 *   by hand rather than left to smart_str's growth heuristic for exactly one
 *   reason: the charge has to happen BEFORE the allocation, and a heuristic
 *   this file cannot predict would have it happening after.
 *
 *   The ordering. In Callback mode a chunk is removed from the buffer before
 *   the callback is invoked with a private copy of it, so a callback that
 *   re-enters the sandbox -- one that evaluates Lua which prints, or that calls
 *   getOutput() -- cannot see or move the bytes still in flight.
 *
 *   The exception contract. The callback is PHP running from inside Lua and it
 *   can throw. EG(exception) is checked explicitly after every call and routed
 *   through luaext_error_raise_from_exception(), which keeps a RuntimeError
 *   catchable and makes anything else fatal, and which carries the original
 *   object rather than its message text.
 *
 * CALLERS MUST KNOW: in Callback mode luaext_output_write() can longjmp, because
 * that is what converting a thrown exception into a Lua error means. A caller
 * that still owns zvals when it writes must release them first, exactly as it
 * would before any other raise. print owns nothing and is safe as written.
 */

#include "luaext_output.h"

#include "luaext_alloc.h"
#include "luaext_error.h"

#include <string.h>

#include <Zend/zend_enum.h>
#include <Zend/zend_exceptions.h>
#include <Zend/zend_operators.h>

/*
 * Mirrors SandboxConfig::$outputChunkBytes in stubs/luaext.stub.php. Repeated
 * here because a SandboxConfig built by reflection can reach construction with
 * the slot still uninitialised, and "the documented default" is a better answer
 * than zero.
 */
#define LUAEXT_OUTPUT_DEFAULT_CHUNK_BYTES ((size_t)8192)

/*
 * Smallest buffer worth allocating. A script that prints one short line at a
 * time would otherwise realloc per write, and every realloc is a charge and a
 * discharge against the memory accounting as well as a copy.
 */
#define LUAEXT_OUTPUT_MIN_CAPACITY ((size_t)256)

/* -------------------------------------------------------------------------
 * Reading the sink's settings off the retained SandboxConfig
 *
 * By name rather than by slot, for the reason luaext_config.c gives: the
 * generated arginfo owns the slot order, and a silent disagreement with it
 * would read every field one place to the left.
 * ---------------------------------------------------------------------- */

/* The declared property's slot, or NULL when the object has no such property. */
static zval *luaext_output_property(const zval *config, const char *name, size_t name_length)
{
	zend_object *object;
	const zend_property_info *info;
	zval *slot;

	if (config == NULL || Z_TYPE_P(config) != IS_OBJECT) {
		return NULL;
	}

	object = Z_OBJ_P(config);
	info = zend_hash_str_find_ptr(&object->ce->properties_info, name, name_length);

	if (info == NULL) {
		return NULL;
	}

	slot = OBJ_PROP(object, info->offset);

	/*
	 * A SandboxConfig built through newInstanceWithoutConstructor() never had
	 * its slots committed. Treating that as "unset" rather than asserting keeps
	 * a reflection-built config from crashing the interpreter.
	 */
	return Z_TYPE_P(slot) == IS_UNDEF ? NULL : slot;
}

#define LUAEXT_OUTPUT_PROPERTY(config, name)                                                       \
	luaext_output_property((config), "" name, sizeof(name) - 1)

/*
 * Enum cases are singletons, so identity is the whole comparison. Anything
 * unrecognisable resolves to Buffer, which is the mode that loses nothing: a
 * sink that cannot be identified must not silently become Discard.
 */
static uint8_t luaext_output_mode_of(const zval *value)
{
	if (value == NULL || Z_TYPE_P(value) != IS_OBJECT) {
		return (uint8_t)LUAEXT_OUTPUT_BUFFER;
	}

	if (Z_OBJ_P(value) == zend_enum_get_case_cstr(luaext_ce_output_mode, "Callback")) {
		return (uint8_t)LUAEXT_OUTPUT_CALLBACK;
	}

	if (Z_OBJ_P(value) == zend_enum_get_case_cstr(luaext_ce_output_mode, "Discard")) {
		return (uint8_t)LUAEXT_OUTPUT_DISCARD;
	}

	return (uint8_t)LUAEXT_OUTPUT_BUFFER;
}

/* -------------------------------------------------------------------------
 * The buffer
 *
 * A smart_str, but grown by hand. luaext_alloc_charge() must be consulted
 * before the memory is taken, not after, and smart_str's own growth policy
 * rounds to a page size this file has no public way to predict. Managing the
 * capacity here keeps one invariant true at every point:
 *
 *     the bytes charged for the output buffer == luaext_output::buf.a
 * ---------------------------------------------------------------------- */

/* Capacity currently charged for. Zero when nothing is allocated. */
static size_t luaext_output_capacity(const luaext_output *out)
{
	return out->buf.s != NULL ? out->buf.a : 0;
}

/*
 * Make room for `extra` more bytes, billing every byte of capacity taken.
 *
 * Returns false when the charge is refused, in which case nothing was allocated
 * and nothing was charged: the caller must fail the write rather than proceed.
 */
static bool luaext_output_reserve(luaext_sandbox *sandbox, size_t extra)
{
	smart_str *buf = &sandbox->out.buf;
	size_t have = luaext_output_capacity(&sandbox->out);
	size_t used = smart_str_get_len(buf);
	size_t needed;
	size_t want;

	if (extra == 0 || have - used >= extra) {
		return true;
	}

	/* A wrap here would under-allocate and then be memcpy'd into. */
	if (extra > SIZE_MAX - used) {
		return false;
	}

	needed = used + extra;
	want = have >= LUAEXT_OUTPUT_MIN_CAPACITY ? have : LUAEXT_OUTPUT_MIN_CAPACITY;

	/* Geometric, so a script printing a line at a time does not realloc per
	 * line; capped at exactly what is needed rather than wrapping. */
	while (want < needed) {
		if (want > SIZE_MAX / 2) {
			want = needed;
			break;
		}

		want *= 2;
	}

	if (!luaext_alloc_charge(sandbox, want - have)) {
		return false;
	}

	/*
	 * Exactly `want`, so the charge and the allocation describe the same
	 * number. zend_string_extend() sets the length to the new capacity, so the
	 * real length is put back straight afterwards.
	 */
	if (buf->s == NULL) {
		buf->s = zend_string_alloc(want, 0);
	} else {
		buf->s = zend_string_extend(buf->s, want, 0);
	}

	ZSTR_LEN(buf->s) = used;
	buf->a = want;

	return true;
}

/* Append into space a successful reserve already accounted for. */
static void luaext_output_append(luaext_output *out, const char *data, size_t length)
{
	size_t used = smart_str_get_len(&out->buf);

	memcpy(ZSTR_VAL(out->buf.s) + used, data, length);
	ZSTR_LEN(out->buf.s) = used + length;
}

/* Drop the first `count` bytes, keeping the capacity (and therefore the charge)
 * for the next write. */
static void luaext_output_consume(luaext_output *out, size_t count)
{
	size_t used = smart_str_get_len(&out->buf);

	if (out->buf.s == NULL) {
		return;
	}

	if (count >= used) {
		ZSTR_LEN(out->buf.s) = 0;
		return;
	}

	memmove(ZSTR_VAL(out->buf.s), ZSTR_VAL(out->buf.s) + count, used - count);
	ZSTR_LEN(out->buf.s) = used - count;
}

/* Give the buffer and its charge back. */
static void luaext_output_release(luaext_sandbox *sandbox)
{
	size_t charged = luaext_output_capacity(&sandbox->out);

	smart_str_free(&sandbox->out.buf);
	luaext_alloc_discharge(sandbox, charged);
}

/* -------------------------------------------------------------------------
 * The callback
 * ---------------------------------------------------------------------- */

/*
 * Hand one chunk to the host, as (string $chunk, bool $isStderr).
 *
 * $isStderr describes the whole chunk, which is why the write path flushes on a
 * channel change rather than letting the two interleave: a chunk carrying both
 * streams could only be labelled by lying about half of it.
 *
 * Returns false with EG(exception) set when the callback threw; the caller
 * decides whether that can be raised into Lua from where it stands.
 */
static bool luaext_output_emit(luaext_sandbox *sandbox, zend_string *chunk)
{
	zval callback_result;
	zval args[2];
	bool called;

	ZVAL_UNDEF(&callback_result);
	ZVAL_STR_COPY(&args[0], chunk);
	ZVAL_BOOL(&args[1], sandbox->out.buffered_is_stderr);

	/* The same counters the PHP bridge keeps: an output callback is a call out
	 * to the host like any other, and stats() should say so. */
	sandbox->php_calls_out++;
	sandbox->in_php++;

	called = call_user_function(NULL, NULL, &sandbox->out.callback, &callback_result, 2, args) ==
			 SUCCESS;

	sandbox->in_php--;

	zval_ptr_dtor(&args[0]);
	zval_ptr_dtor(&callback_result);

	return called && EG(exception) == NULL;
}

/*
 * Push what the callback is owed.
 *
 * `all` sends everything pending -- the end of a run, or a host asking what
 * there is. Otherwise only whole chunks and completed lines go, which is what
 * makes a streaming host see line-aligned output without waiting for the
 * chunk threshold.
 *
 * Returns false with EG(exception) set when the callback threw. Nothing this
 * function owns survives that return, so the caller may raise.
 */
static bool luaext_output_flush(luaext_sandbox *sandbox, bool all)
{
	luaext_output *out = &sandbox->out;

	if (out->mode != (uint8_t)LUAEXT_OUTPUT_CALLBACK || Z_TYPE(out->callback) == IS_UNDEF) {
		return true;
	}

	for (;;) {
		size_t pending = smart_str_get_len(&out->buf);
		const char *start;
		zend_string *chunk;
		size_t take;
		bool emitted;

		if (pending == 0) {
			break;
		}

		start = ZSTR_VAL(out->buf.s);

		if (all || out->chunk == 0) {
			/* chunk == 0 is the host asking for no buffering at all. */
			take = pending;
		} else if (pending >= out->chunk) {
			take = out->chunk;
		} else {
			const char *newline = (const char *)zend_memrchr(start, '\n', pending);

			if (newline == NULL) {
				break;
			}

			take = (size_t)(newline - start) + 1;
		}

		/*
		 * Copied out and consumed BEFORE the call. The callback is host code
		 * that can re-enter this sandbox, and a re-entrant write would realloc
		 * the buffer `start` points into.
		 */
		chunk = zend_string_init(start, take, 0);
		luaext_output_consume(out, take);

		emitted = luaext_output_emit(sandbox, chunk);
		zend_string_release(chunk);

		if (!emitted) {
			return false;
		}
	}

	if (all) {
		luaext_output_release(sandbox);
	}

	return true;
}

/*
 * Convert the exception a callback threw into a Lua error, if there is anywhere
 * to raise it to. Does not return when it raises.
 *
 * Outside the interpreter -- a flush from getOutput(), or from close() -- there
 * is no protected call to unwind to and lua_error() would reach lua_atpanic and
 * take the request with it. The exception is left pending for PHP instead,
 * which is where the host already is.
 */
static void luaext_output_report_exception(luaext_sandbox *sandbox)
{
	lua_State *L = sandbox->running_L != NULL ? sandbox->running_L : sandbox->L;

	if (EG(exception) == NULL || L == NULL || sandbox->in_lua <= 0) {
		return;
	}

	/*
	 * A RuntimeError stays catchable and anything else becomes fatal, and either
	 * way the original object is carried rather than its message text. That
	 * judgement belongs to the error subsystem, not here.
	 */
	luaext_error_raise_from_exception(L);
}

/* -------------------------------------------------------------------------
 * Lifecycle
 * ---------------------------------------------------------------------- */

bool luaext_output_init(luaext_sandbox *sandbox, zval *config)
{
	luaext_output *out = &sandbox->out;
	const zval *mode_value = LUAEXT_OUTPUT_PROPERTY(config, "outputMode");
	const zval *chunk_value = LUAEXT_OUTPUT_PROPERTY(config, "outputChunkBytes");
	zval *callback = LUAEXT_OUTPUT_PROPERTY(config, "outputCallback");
	uint8_t mode = luaext_output_mode_of(mode_value);
	size_t chunk = LUAEXT_OUTPUT_DEFAULT_CHUNK_BYTES;

	if (chunk_value != NULL && Z_TYPE_P(chunk_value) == IS_LONG) {
		/*
		 * SandboxConfig does not range-check this one, so it is checked at the
		 * first moment anything depends on it. A negative chunk size would
		 * become an enormous size_t and buffer the whole run.
		 */
		if (Z_LVAL_P(chunk_value) < 0) {
			zend_throw_exception_ex(luaext_ce_configuration_error, 0,
									"SandboxConfig::$outputChunkBytes is " ZEND_LONG_FMT
									", which is not a number of bytes to buffer. Pass 0 to hand "
									"every write straight to the callback.",
									Z_LVAL_P(chunk_value));
			return false;
		}

		chunk = (size_t)Z_LVAL_P(chunk_value);
	}

	if (mode == (uint8_t)LUAEXT_OUTPUT_CALLBACK &&
		(callback == NULL || Z_TYPE_P(callback) != IS_OBJECT)) {
		zend_throw_exception(luaext_ce_configuration_error,
							 "OutputMode::Callback needs a SandboxConfig::$outputCallback to "
							 "stream to. Without one a script's output would go nowhere, which "
							 "OutputMode::Discard says deliberately.",
							 0);
		return false;
	}

	/* Committed only once nothing can still refuse: a sandbox whose
	 * construction throws is still torn down, and shutdown reads these. */
	out->mode = mode;
	out->chunk = chunk;
	out->limit = sandbox->policy.limits.output_bytes;
	out->written = 0;
	out->truncated = false;

	/*
	 * Our own reference, rather than reaching back through sandbox->config_zv
	 * on every write. The retained config would in fact outlive the sink --
	 * close() runs luaext_output_shutdown() before releasing it -- but a sink
	 * that owns what it calls does not depend on that ordering staying true,
	 * and it costs one refcount for the life of the sandbox.
	 */
	if (mode == (uint8_t)LUAEXT_OUTPUT_CALLBACK) {
		ZVAL_COPY(&out->callback, callback);
	}

	return true;
}

void luaext_output_shutdown(luaext_sandbox *sandbox)
{
	luaext_output *out = &sandbox->out;

	/*
	 * Best effort. This runs from close(), from the object destructor and from
	 * the RSHUTDOWN sweep, and none of them can act on a failure: a callback
	 * that throws on the way out leaves its exception pending for the host and
	 * the rest of the teardown carries on regardless.
	 */
	(void)luaext_output_flush(sandbox, true);

	luaext_output_release(sandbox);

	if (Z_TYPE(out->callback) != IS_UNDEF) {
		zval_ptr_dtor(&out->callback);
		ZVAL_UNDEF(&out->callback);
	}
}

/* -------------------------------------------------------------------------
 * Writing
 * ---------------------------------------------------------------------- */

luaext_output_status luaext_output_write(luaext_sandbox *sandbox, const char *data, size_t length)
{
	return luaext_output_write_channel(sandbox, data, length, false);
}

luaext_output_status luaext_output_write_channel(luaext_sandbox *sandbox, const char *data,
												 size_t length, bool is_stderr)
{
	luaext_output *out;
	size_t accepted;
	bool overflowed;

	/* Teardown can still reach a print from a finaliser. Nothing to record and
	 * nothing to report. */
	if (sandbox == NULL || sandbox->closed) {
		return LUAEXT_OUTPUT_ACCEPTED;
	}

	out = &sandbox->out;

	if (data == NULL || length == 0) {
		return LUAEXT_OUTPUT_ACCEPTED;
	}

	/*
	 * A channel change flushes what is pending before the new bytes join it.
	 * Without this a print() followed by an io.stderr:write() would arrive as
	 * one chunk carrying one flag, and whichever flag it carried would be wrong
	 * about the other half.
	 */
	if (is_stderr != out->buffered_is_stderr && smart_str_get_len(&out->buf) > 0) {
		if (!luaext_output_flush(sandbox, true)) {
			/* An exception is pending; the caller reports it. ACCEPTED so no
			 * second error is raised over the real one. */
			return LUAEXT_OUTPUT_ACCEPTED;
		}
	}

	out->buffered_is_stderr = is_stderr;

	/*
	 * What fits. Reaching the cap exactly is not an overflow -- outputBytes is
	 * how much a script MAY emit -- so only the byte past it is refused.
	 */
	accepted = length;
	overflowed = false;

	if (out->limit != 0) {
		size_t remaining = out->written < out->limit ? out->limit - out->written : 0;

		if (length > remaining) {
			accepted = remaining;
			overflowed = true;
		}
	}

	/*
	 * The counter records what the script EMITTED, not what survived. A budget
	 * report that shrank when the sink dropped the excess would tell a host its
	 * script behaved, which is the opposite of what happened. Saturating for
	 * the same reason: a wrap would read as compliance.
	 */
	out->written = length > SIZE_MAX - out->written ? SIZE_MAX : out->written + length;

	if (overflowed) {
		/* True under both behaviours: output was dropped either way, and a host
		 * that catches the OutputLimitError still wants to know it. */
		out->truncated = true;
	}

	if (accepted > 0 && out->mode != (uint8_t)LUAEXT_OUTPUT_DISCARD) {
		if (!luaext_output_reserve(sandbox, accepted)) {
			/* The memory budget, not the output budget, refused this. Saying so
			 * is the point of the tri-state: the two limits are tuned
			 * separately and the old shared `false` blamed the wrong one. */
			out->truncated = true;
			return LUAEXT_OUTPUT_REFUSED_MEMORY;
		}

		luaext_output_append(out, data, accepted);
	}

	/*
	 * Push whatever the callback is now owed, whether or not this write
	 * overflowed: the bytes accepted before the cap are still the script's
	 * output and the host is still owed them in order.
	 */
	if (!luaext_output_flush(sandbox, false)) {
		/* Normally raises the callback's own exception and never returns. The
		 * exception-free failure -- call_user_function refusing a callback
		 * validated at construction -- is close enough to unreachable that it
		 * keeps the budget error rather than growing a fourth status. */
		luaext_output_report_exception(sandbox);
		return LUAEXT_OUTPUT_REFUSED_BUDGET;
	}

	if (!overflowed) {
		return LUAEXT_OUTPUT_ACCEPTED;
	}

	/*
	 * The one decision this file exists to make. Truncate has already recorded
	 * the loss, so the script carries on none the wiser; Fail reports the spent
	 * budget and the caller raises a fatal OutputLimitError, which is the only
	 * form a script cannot pcall its way past.
	 */
	return sandbox->policy.limits.output_overflow == (uint8_t)LUAEXT_OVERFLOW_TRUNCATE
			   ? LUAEXT_OUTPUT_ACCEPTED
			   : LUAEXT_OUTPUT_REFUSED_BUDGET;
}

/* -------------------------------------------------------------------------
 * What the host reads back
 * ---------------------------------------------------------------------- */

zend_string *luaext_output_get(luaext_sandbox *sandbox, bool take)
{
	luaext_output *out = &sandbox->out;
	zend_string *result;
	size_t charged;

	/*
	 * Everything the callback is owed goes out before the host is told what
	 * there is, so a host that mixes the two sees one ordering rather than two.
	 */
	if (!luaext_output_flush(sandbox, true)) {
		/* An exception is pending; the caller turns a NULL into RETURN_THROWS. */
		return NULL;
	}

	/*
	 * Discard kept nothing and Callback already streamed it. An empty string,
	 * not an error: the host asked where the output is and "nowhere
	 * retrievable" is a true answer, whereas throwing would make a mode switch
	 * break code that merely reads its own output back.
	 */
	if (out->mode != (uint8_t)LUAEXT_OUTPUT_BUFFER) {
		return ZSTR_EMPTY_ALLOC();
	}

	if (out->buf.s == NULL) {
		return ZSTR_EMPTY_ALLOC();
	}

	if (!take) {
		return zend_string_init(ZSTR_VAL(out->buf.s), smart_str_get_len(&out->buf), 0);
	}

	charged = luaext_output_capacity(out);

	smart_str_0(&out->buf);
	result = smart_str_extract(&out->buf);

	/* smart_str_extract() hands the string over and clears the pointer, but
	 * leaves the recorded capacity behind. */
	out->buf.a = 0;
	luaext_alloc_discharge(sandbox, charged);

	/*
	 * The byte count goes with the bytes, so a host that drains in a loop gives
	 * the script its budget back a batch at a time. The truncation flag does
	 * NOT: a host that took the output still needs to know it was incomplete.
	 */
	out->written = 0;

	return result;
}

void luaext_output_set_limit(luaext_sandbox *sandbox, size_t bytes)
{
	/*
	 * The written count is deliberately left alone. A host lowering the budget
	 * is narrowing what a script may still emit, not declaring that the bytes
	 * already emitted did not happen -- and refunding them would let a script
	 * with reach into its own sandbox spend the budget over and over.
	 */
	sandbox->out.limit = bytes;
}
