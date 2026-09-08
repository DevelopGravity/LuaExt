/*
 * luaext — the PHP side of the boundary.
 *
 * One rule shapes this file, and it is the one the docs promise: a callback
 * that throws a RuntimeError is the host saying "the script is meant to handle
 * this", so the script may catch it; a callback that throws anything else is a
 * failure the script had no part in and must not be able to swallow. Both
 * travel as the *exception object*, never as its message text, so the host
 * catches the class it threw rather than a string that used to be one. The
 * extension this replaces kept only the message, which is why a host could no
 * longer tell a database outage from a validation failure.
 *
 * The second rule is about unwinding. lua_error() is a longjmp: it runs no C
 * cleanup, so a frame holding a zval, an emalloc'd array or a borrowed
 * fcall_info_cache must release all of it before raising. Every function here
 * is arranged so that the raise is the last thing it does, and debug builds
 * assert it through LUAEXT_NO_RAISE_BEGIN/END.
 *
 * The third is ownership. A registered callable outlives the call that
 * registered it, so its fcall_info_cache lives in the closure's own userdata
 * and is released by that userdata's __gc. Lua decides when a closure dies;
 * borrowing the caller's fcc would be a use-after-free the first time it did.
 */

#include "luaext_phpcall.h"

#include "luaext_alloc.h"
#include "luaext_convert.h"
#include "luaext_defer.h"
#include "luaext_error.h"
#include "luaext_clock.h"
#include "luaext_exec.h"
#include "luaext_timers.h"

#include <lauxlib.h>
#include <lua.h>

#include <string.h>

#include <Zend/zend_attributes.h>
#include <Zend/zend_exceptions.h>
#include <Zend/zend_smart_str.h>

/*
 * Identifies the closure storage. Read before anything else in the userdata is
 * touched, so a userdata belonging to some other subsystem is left alone rather
 * than reinterpreted.
 */
#define LUAEXT_PHPCALL_MAGIC 0x4C58436Bu /* "LXCk" */

/* Stack slots any one step here needs: a function, two arguments, a result. */
#define LUAEXT_PHPCALL_SLOTS 8

/* What an unnamed callable is called in a message. */
#define LUAEXT_PHPCALL_ANONYMOUS "an anonymous host callback"

/* How long a callback must have run before its boundary samples the deadline
 * directly; see the comment at the check. One scheduler quantum, roughly. */
#define LUAEXT_PHPCALL_SAMPLE_AFTER_NS ((uint64_t)1000000)

/* -------------------------------------------------------------------------
 * Closure storage
 *
 * A full userdata rather than a light one: only a full userdata can carry a
 * __gc, and __gc is the only thing that can tell us when Lua has finished with
 * a callable the host handed over.
 * ---------------------------------------------------------------------- */

typedef struct {
	uint32_t magic;

	/*
	 * Owned. zend_fcc_dup() takes references on the bound object and closure
	 * and copies a trampoline out of EG(trampoline) if the callable resolved to
	 * one, so this survives the call that registered it and stays valid across
	 * repeated invocations.
	 */
	zend_fcall_info_cache fcc;

	/*
	 * The name the host gave this callable, for messages. Persistent rather
	 * than request-allocated for the same reason the error subsystem's message
	 * is: __gc also runs from lua_close() during the request-shutdown sweep.
	 */
	zend_string *name;
} luaext_phpcall_ud;

/*
 * Release what the storage owns outside the Lua heap.
 *
 * Runs from the collector and from lua_close(). Releasing the fcc can drop the
 * last reference to the bound object and therefore run a PHP destructor; see
 * the note on re-entrancy in luaext_phpcall_metatable().
 */
static int luaext_phpcall_release(lua_State *L)
{
	luaext_phpcall_ud *slot = (luaext_phpcall_ud *)lua_touserdata(L, 1);

	if (slot == NULL || slot->magic != LUAEXT_PHPCALL_MAGIC) {
		return 0;
	}

	/* A finalised closure is no longer one of ours, even if it is resurrected. */
	slot->magic = 0;

	if (ZEND_FCC_INITIALIZED(slot->fcc)) {
		/*
		 * Handed to the deferred queue rather than released here. Releasing can
		 * drop the last reference to the bound object and run its __destruct,
		 * which is arbitrary host code, and this frame is inside the collector
		 * of the state that code is free to call back into. See luaext_defer.h.
		 * With no sandbox left to drain a queue, release directly and accept
		 * the narrow window -- leaking the reference would be worse.
		 */
		luaext_sandbox *sandbox = LUAEXT_SB(L);

		if (sandbox != NULL) {
			luaext_defer_fcc(sandbox, &slot->fcc);
		} else {
			zend_fcc_dtor(&slot->fcc);
		}
	}

	if (slot->name != NULL) {
		zend_string_release(slot->name);
		slot->name = NULL;
	}

	return 0;
}

/*
 * Push the metatable shared by every closure storage in this state, creating it
 * on first use.
 *
 * A metatable *is* its __gc, so the registry key names one payload type: a
 * userdata carrying this metatable must be a luaext_phpcall_ud. A subsystem
 * that wants to hang a different payload off Lua's collector needs its own key,
 * because handing its userdata this __gc would silently leak whatever that
 * payload owns.
 *
 * __metatable is set for the same reason the error subsystem sets it: a script
 * that could read this table back out could replace __gc, and a script that
 * could stamp it onto a value of its own could hand the collector a pointer to
 * memory we never allocated.
 */
static void luaext_phpcall_metatable(lua_State *L)
{
	if (lua_rawgetp(L, LUA_REGISTRYINDEX, &luaext_key_zvalmt) == LUA_TTABLE) {
		return;
	}

	lua_pop(L, 1);
	lua_createtable(L, 0, 2);

	lua_pushcfunction(L, luaext_phpcall_release);
	lua_setfield(L, -2, "__gc");

	lua_pushboolean(L, 0);
	lua_setfield(L, -2, "__metatable");

	lua_pushvalue(L, -1);
	lua_rawsetp(L, LUA_REGISTRYINDEX, &luaext_key_zvalmt);
}

/* What to call this callable in a message. Never NULL. */
static const char *luaext_phpcall_label(const luaext_phpcall_ud *slot)
{
	if (slot == NULL || slot->name == NULL || ZSTR_LEN(slot->name) == 0) {
		return LUAEXT_PHPCALL_ANONYMOUS;
	}

	return ZSTR_VAL(slot->name);
}

/* -------------------------------------------------------------------------
 * Calling PHP from Lua
 * ---------------------------------------------------------------------- */

/*
 * Convert the callback's return value onto the Lua stack, under a protected
 * call.
 *
 * luaext_convert_push_zval() reports failure by raising, and the caller is
 * still holding the return value and the argument array when it does. Running
 * it here means that raise unwinds no further than the caller's own lua_pcall,
 * which then releases everything and re-raises deliberately.
 *
 * Arguments: 1 = the owning sandbox, 2 = the zval to convert.
 */
static int luaext_phpcall_push_result(lua_State *L)
{
	luaext_sandbox *sandbox = (luaext_sandbox *)lua_touserdata(L, 1);
	zval *result = (zval *)lua_touserdata(L, 2);

	lua_settop(L, 0);
	luaext_convert_push_zval(sandbox, L, result);

	return 1;
}

/* -------------------------------------------------------------------------
 * The strict argument gate
 *
 * Arguments a script hands a typed PHP callable are held to the callee's
 * declared signature under strict_types=1 semantics BEFORE the call — the
 * engine's own coercion never runs for internal-origin calls under the
 * caller's strictness, so without this gate the boundary silently juggles
 * ("42" becomes int, 1 becomes "1") depending on nothing the script or the
 * callee wrote. Two deliberate tightenings beyond the engine: surplus
 * arguments to a non-variadic callee are refused rather than parked in
 * func_get_args(), and every contract mismatch is a CATCHABLE Lua error —
 * the script's mistake, named where the script can adapt to it.
 * ---------------------------------------------------------------------- */

typedef enum {
	LUAEXT_PHPCALL_ARGS_OK,
	LUAEXT_PHPCALL_ARGS_MISMATCH,  /* message formatted; raise after cleanup */
	LUAEXT_PHPCALL_ARGS_EXCEPTION, /* a check ran PHP that threw; EG(exception) rules */
} luaext_phpcall_args_verdict;

/*
 * self/parent in a parameter type resolve against the DECLARING function's
 * scope: a plain class's `self` is already compile-time resolved to its name,
 * but one declared in a trait stays literal. Everything else is a no-autoload
 * lookup — an instance of a class that is not loaded cannot exist, so a
 * failed lookup IS a mismatch, and no user PHP runs deciding it.
 */
static zend_class_entry *luaext_phpcall_resolve_type_name(const zend_function *fn,
														  zend_string *name)
{
	if (zend_string_equals_literal_ci(name, "self")) {
		return fn->common.scope;
	}

	if (zend_string_equals_literal_ci(name, "parent")) {
		return fn->common.scope != NULL ? fn->common.scope->parent : NULL;
	}

	return zend_lookup_class_ex(name, NULL, ZEND_FETCH_CLASS_NO_AUTOLOAD);
}

/*
 * Render a declared type for a refusal message, with `self`/`parent` resolved
 * against the callee's scope the way the engine's own TypeError names them.
 *
 * The engine's resolved renderer (zend_type_to_string_resolved) carries no
 * ZEND_API marker, so an extension cannot rely on the symbol -- macOS loads it
 * as NULL under -undefined suppress, and a Windows DLL will not link at all.
 * Resolved here instead, on the rendered form: the two words are reserved, so
 * a whole segment spelling one can only be the type token, never a class name.
 */
static zend_string *luaext_phpcall_type_string(const zend_function *fn, zend_type type)
{
	zend_string *rendered = zend_type_to_string(type);
	const zend_class_entry *scope = fn->common.scope;
	const char *cursor = ZSTR_VAL(rendered);
	const char *end = cursor + ZSTR_LEN(rendered);
	smart_str out = {0};

	if (scope == NULL || (strstr(ZSTR_VAL(rendered), "self") == NULL &&
						  strstr(ZSTR_VAL(rendered), "parent") == NULL)) {
		return rendered;
	}

	while (cursor < end) {
		const char *start = cursor;
		size_t token_len;

		while (cursor < end && *cursor != '|' && *cursor != '&' && *cursor != '?' &&
			   *cursor != '(' && *cursor != ')') {
			cursor++;
		}

		token_len = (size_t)(cursor - start);

		if (token_len == 4 && zend_binary_strcasecmp(start, 4, "self", 4) == 0) {
			smart_str_append(&out, scope->name);
		} else if (token_len == 6 && zend_binary_strcasecmp(start, 6, "parent", 6) == 0 &&
				   scope->parent != NULL) {
			smart_str_append(&out, scope->parent->name);
		} else {
			smart_str_appendl(&out, start, token_len);
		}

		if (cursor < end) {
			smart_str_appendc(&out, *cursor);
			cursor++;
		}
	}

	zend_string_release(rendered);
	smart_str_0(&out);

	return out.s != NULL ? out.s : ZSTR_EMPTY_ALLOC();
}

/* One union member (a class name, or an intersection list of them) against
 * the argument's class. DNF shapes nest exactly one level. */
static bool luaext_phpcall_type_member_matches(const zend_function *fn, const zend_type *member,
											   zend_class_entry *object_ce)
{
	if (ZEND_TYPE_HAS_LIST(*member)) {
		const zend_type *sub;

		ZEND_TYPE_LIST_FOREACH(ZEND_TYPE_LIST(*member), sub)
		{
			if (!luaext_phpcall_type_member_matches(fn, sub, object_ce)) {
				return false;
			}
		}
		ZEND_TYPE_LIST_FOREACH_END();

		return true;
	}

	if (ZEND_TYPE_HAS_NAME(*member)) {
		zend_class_entry *ce = luaext_phpcall_resolve_type_name(fn, ZEND_TYPE_NAME(*member));

		return ce != NULL && instanceof_function(object_ce, ce);
	}

	if (ZEND_TYPE_HAS_LITERAL_NAME(*member)) {
		/* Internal/frameless shapes carry the name as a C string. */
		const char *raw = ZEND_TYPE_LITERAL_NAME(*member);
		zend_string *name = zend_string_init(raw, strlen(raw), 0);
		zend_class_entry *ce = luaext_phpcall_resolve_type_name(fn, name);

		zend_string_release(name);

		return ce != NULL && instanceof_function(object_ce, ce);
	}

	return false;
}

/* An object argument against the declared class portion of a type. */
static bool luaext_phpcall_object_matches(const zend_function *fn, const zend_type *type,
										  zend_class_entry *object_ce)
{
	if (!ZEND_TYPE_IS_COMPLEX(*type)) {
		return false;
	}

	if (ZEND_TYPE_HAS_LIST(*type)) {
		const zend_type *member;

		if (ZEND_TYPE_IS_INTERSECTION(*type)) {
			ZEND_TYPE_LIST_FOREACH(ZEND_TYPE_LIST(*type), member)
			{
				if (!luaext_phpcall_type_member_matches(fn, member, object_ce)) {
					return false;
				}
			}
			ZEND_TYPE_LIST_FOREACH_END();

			return true;
		}

		ZEND_TYPE_LIST_FOREACH(ZEND_TYPE_LIST(*type), member)
		{
			if (luaext_phpcall_type_member_matches(fn, member, object_ce)) {
				return true;
			}
		}
		ZEND_TYPE_LIST_FOREACH_END();

		return false;
	}

	return luaext_phpcall_type_member_matches(fn, type, object_ce);
}

/*
 * One argument against one declared type, strictly.
 *
 * The scalar tail is OURS on purpose: zend_check_user_type_slow()'s fallback
 * reads the AMBIENT frame's strict_types — whichever host file happened to
 * call eval() — and this boundary's strictness must not depend on that.
 * zend_verify_scalar_type_hint(strict=true) is the engine's exact
 * strict_types=1 rule, including its one sanctioned widening (int -> float,
 * converted in place; the result is a non-refcounted double, so a LATER
 * argument refusing strands nothing).
 *
 * `ran_php` reports that zend_is_callable ran — the one branch that can
 * autoload and reach user error handlers; the caller re-checks EG(exception).
 */
static bool luaext_phpcall_arg_matches(const zend_function *fn, const zend_type *type, zval *arg,
									   bool *ran_php)
{
	uint32_t mask = ZEND_TYPE_FULL_MASK(*type);

	if (Z_TYPE_P(arg) == IS_OBJECT) {
		if (luaext_phpcall_object_matches(fn, type, Z_OBJCE_P(arg))) {
			return true;
		}

		/* IS_CALLABLE_SUPPRESS_DEPRECATIONS because this is a CHECK, not a
		 * call: the engine's own arg verification suppresses them too, and the
		 * dispatch that follows will surface any deprecation exactly once. */
		if ((mask & MAY_BE_CALLABLE) != 0) {
			*ran_php = true;
			return zend_is_callable(arg, IS_CALLABLE_SUPPRESS_DEPRECATIONS, NULL);
		}

		return false;
	}

	if ((mask & MAY_BE_CALLABLE) != 0 &&
		(Z_TYPE_P(arg) == IS_STRING || Z_TYPE_P(arg) == IS_ARRAY)) {
		*ran_php = true;

		if (zend_is_callable(arg, IS_CALLABLE_SUPPRESS_DEPRECATIONS, NULL)) {
			return true;
		}
	}

	return zend_verify_scalar_type_hint(mask, arg, true, false);
}

/* The parameter's Lua-facing name, mindful of the arg_info union: user
 * functions carry zend_string names, internal ones C strings. */
static const char *luaext_phpcall_arg_name(const zend_function *fn, const zend_arg_info *info)
{
	if (fn->type == ZEND_USER_FUNCTION) {
		return info->name != NULL ? ZSTR_VAL(info->name) : "?";
	}

	{
		const char *name = ((const zend_internal_arg_info *)info)->name;

		return name != NULL ? name : "?";
	}
}

/*
 * Arity and per-argument types for the call about to be made. On MISMATCH the
 * refusal is formatted into `message` and raised by the caller once the frame
 * owns nothing; nothing here raises. Runs no user PHP except the callable
 * check, whose thrown exception becomes the EXCEPTION verdict and routes
 * through the boundary's ordinary exception path.
 */
static luaext_phpcall_args_verdict luaext_phpcall_check_args(const luaext_phpcall_target *target,
															 zval *params, uint32_t argc,
															 const char *label, char *message,
															 size_t message_size)
{
	const zend_function *fn = target->fcc != NULL ? target->fcc->function_handler : target->fn;
	uint32_t required = fn->common.required_num_args;
	uint32_t declared = fn->common.num_args;
	bool variadic = (fn->common.fn_flags & ZEND_ACC_VARIADIC) != 0;
	uint32_t index;

	if (argc < required) {
		snprintf(message, message_size, "%s expects at least %u argument(s), %u given", label,
				 (unsigned int)required, (unsigned int)argc);
		return LUAEXT_PHPCALL_ARGS_MISMATCH;
	}

	/* No declared signature (a __call trampoline): the floor above is all
	 * there is to hold the call to. */
	if (fn->common.arg_info == NULL) {
		return LUAEXT_PHPCALL_ARGS_OK;
	}

	if (!variadic && argc > declared) {
		snprintf(message, message_size, "%s expects at most %u argument(s), %u given", label,
				 (unsigned int)declared, (unsigned int)argc);
		return LUAEXT_PHPCALL_ARGS_MISMATCH;
	}

	for (index = 0; index < argc; index++) {
		/* Surplus args of a variadic callee validate against its slot. */
		const zend_arg_info *info = &fn->common.arg_info[index < declared ? index : declared];
		bool ran_php = false;

		/* Only a hard by-ref parameter cannot cross. Prefer-ref (array_multisort's
		 * arrays, sort's array) accepts a plain value, which is exactly what
		 * zend_call_function passes for a non-reference -- refusing those would
		 * turn callables the engine happily runs into boundary errors. */
		if (ZEND_ARG_SEND_MODE(info) == ZEND_SEND_BY_REF) {
			snprintf(message, message_size,
					 "%s: argument #%u ($%s) is passed by reference, which cannot cross from "
					 "Lua",
					 label, (unsigned int)(index + 1), luaext_phpcall_arg_name(fn, info));
			return LUAEXT_PHPCALL_ARGS_MISMATCH;
		}

		if (!ZEND_TYPE_IS_SET(info->type) ||
			ZEND_TYPE_CONTAINS_CODE(info->type, Z_TYPE(params[index]))) {
			continue;
		}

		if (luaext_phpcall_arg_matches(fn, &info->type, &params[index], &ran_php)) {
			continue;
		}

		if (ran_php && EG(exception) != NULL) {
			return LUAEXT_PHPCALL_ARGS_EXCEPTION;
		}

		{
			/* Resolved against the callee's scope, so a method typed `self`
			 * names its class the way the engine's own TypeError would. */
			zend_string *expected = luaext_phpcall_type_string(fn, info->type);

			snprintf(message, message_size, "%s: argument #%u ($%s) must be of type %s, %s given",
					 label, (unsigned int)(index + 1), luaext_phpcall_arg_name(fn, info),
					 ZSTR_VAL(expected), zend_zval_value_name(&params[index]));
			zend_string_release(expected);
		}

		return LUAEXT_PHPCALL_ARGS_MISMATCH;
	}

	return LUAEXT_PHPCALL_ARGS_OK;
}

/*
 * The boundary core every host call goes through — registered callables via
 * the closure below it, proxy dispatch (methods, statics, operators)
 * directly. Exactly one of target->fcc / target->fn is set.
 *
 * One PHP return value becomes one Lua value: a string returns a string, an
 * array returns a table. The extension this replaces required a callback to
 * wrap even a single result in an array and warned if it did not, which made
 * the common case the awkward one; a script that genuinely wants several values
 * out of one call unpacks the table it was given.
 */
int luaext_phpcall_invoke_target(lua_State *L, const luaext_phpcall_target *target)
{
	luaext_sandbox *sandbox = LUAEXT_SB(L);
	const char *label = target->label != NULL ? target->label : LUAEXT_PHPCALL_ANONYMOUS;
	uint32_t depth_limit;
	zval *params = NULL;
	size_t params_bytes = 0;
	size_t content_bytes = 0;
	zval result;
	int argc = lua_gettop(L) - (target->first_arg - 1);
	int index;
	int status = LUA_OK;
	bool converted = true;
	luaext_phpcall_args_verdict verdict = LUAEXT_PHPCALL_ARGS_OK;
	char gate_message[384];
	uint64_t call_started_ns = 0;
	luaext_host_span host_span;

	/*
	 * Everything up to the argument loop owns nothing, so it may raise freely.
	 * Past it, the frame owns zvals and host memory and must not.
	 */

	if (argc < 0) {
		argc = 0;
	}

	if (sandbox == NULL || sandbox->closed || sandbox->L == NULL) {
		luaext_error_raise(L, LUAEXT_ERR_ABORT, true,
						   "The host callback %s cannot run: its sandbox is gone", label);
	}

	/*
	 * An exception already in flight would be indistinguishable from one this
	 * callback threw, and calling PHP with one pending is undefined anyway.
	 * Converting it here keeps the classification honest.
	 */
	if (EG(exception) != NULL) {
		luaext_error_raise_from_exception(L);
	}

	/*
	 * Bounds how deeply Lua and PHP may call each other. The interpreter's own
	 * C-call ceiling would eventually stop unbounded recursion as an untyped
	 * "C stack overflow", so this is not the only guard -- but it is the one the
	 * host configured, and it names what actually happened.
	 */
	depth_limit = sandbox->policy.limits.max_call_depth;

	if (depth_limit != 0 && sandbox->in_php >= 0 && (uint32_t)sandbox->in_php >= depth_limit) {
		luaext_error_raise(L, LUAEXT_ERR_ABORT, true,
						   "The host callback %s was refused: calls across the PHP boundary are "
						   "already nested %u deep",
						   label, depth_limit);
	}

	if (!lua_checkstack(L, LUAEXT_PHPCALL_SLOTS)) {
		luaext_error_raise(L, LUAEXT_ERR_ABORT, true,
						   "The host callback %s was refused: the interpreter stack cannot grow",
						   label);
	}

	/*
	 * The argument array is host memory the script caused to be allocated, and
	 * lua_Alloc never sees it. Billing it against the same ceiling is what stops
	 * a script from spending host memory it has no budget for by calling out
	 * with an enormous argument list.
	 */
	if (argc > 0) {
		params_bytes = (size_t)argc * sizeof(zval);

		if (!luaext_alloc_charge(sandbox, params_bytes)) {
			luaext_error_raise(L, LUAEXT_ERR_MEMORY, true,
							   "The host callback %s was refused: its %d argument(s) do not fit in "
							   "the sandbox's memory budget",
							   label, argc);
		}

		params = (zval *)safe_emalloc((size_t)argc, sizeof(zval), 0);
	}

	ZVAL_UNDEF(&result);

	LUAEXT_NO_RAISE_BEGIN(L);

	/* Valid before anything can fail, so every slot is releasable either way. */
	for (index = 0; index < argc; index++) {
		ZVAL_UNDEF(&params[index]);
	}

	/*
	 * Billed, unlike the conversions that hand a value onward to PHP: these
	 * zvals are ours and are released below, so the charge has somewhere to be
	 * given back. Without it a script can hand a callback a large string or
	 * table and make the process hold a second copy that memoryBytes never saw.
	 */
	for (index = 0; index < argc && converted; index++) {
		size_t billed = 0;

		converted = luaext_convert_to_zval_billed(sandbox, L, target->first_arg + index,
												  &params[index], &billed);

		/* Accumulated even when that call failed: a partial conversion has
		 * already charged for the part it built. */
		content_bytes += billed;
	}

	/*
	 * The strict gate, between conversion and dispatch: arity and each
	 * argument's declared type, under strict_types=1 semantics. A refused
	 * call is skipped entirely — never billed as a crossing, never counted.
	 */
	if (converted) {
		bool gate_paused = false;

		/*
		 * The gate's callable check is the one branch that can run host PHP —
		 * an autoloader, an error handler — and that work is the host's, not
		 * the script's: the same in_php frame and billHostTime pause the
		 * dispatch below applies. Without this, a slow autoloader fired by the
		 * check was charged to the script's clocks, on a call that might then
		 * never even be made.
		 */
		sandbox->in_php++;

		if (!sandbox->policy.limits.bill_host_time) {
			gate_paused = luaext_timers_pause(sandbox, LUAEXT_TIMER_CPU | LUAEXT_TIMER_WALL);
		}

		verdict = luaext_phpcall_check_args(target, params, (uint32_t)argc, label, gate_message,
											sizeof(gate_message));

		sandbox->in_php--;

		/*
		 * The check's own PHP can throw and still answer "callable" — a user
		 * error handler, most easily. The engine refuses to run anything with
		 * an exception pending, so that crossing will never happen and must
		 * not be billed as one.
		 */
		if (verdict == LUAEXT_PHPCALL_ARGS_OK && EG(exception) != NULL) {
			verdict = LUAEXT_PHPCALL_ARGS_EXCEPTION;
		}

		/*
		 * An OK verdict hands its pause straight to the dispatch below — the
		 * pause there is an idempotent flag, and luaext_timers_php_returned
		 * reopens it as always. Any other verdict skips dispatch, so the
		 * clocks reopen here.
		 */
		if (gate_paused && verdict != LUAEXT_PHPCALL_ARGS_OK) {
			luaext_timers_resume(sandbox, LUAEXT_TIMER_CPU | LUAEXT_TIMER_WALL);
		}
	}

	if (converted && verdict == LUAEXT_PHPCALL_ARGS_OK) {
		/*
		 * The boundary counters. in_php is what lets a later wave stop charging
		 * CPU to the script while the host works, and what makes a nested
		 * callback's depth accounting correct; php_calls_out is what stats()
		 * reports.
		 */
		sandbox->php_calls_out++;
		sandbox->in_php++;

		/*
		 * Limits::$billHostTime off means every crossing pauses both clocks,
		 * the same pause the callback could take itself with pauseTimers() and
		 * under the same rules. Nothing here resumes: luaext_timers_php_returned
		 * below reopens whatever is paused, exactly as it already does for a
		 * callback that paused and forgot.
		 */
		if (!sandbox->policy.limits.bill_host_time) {
			(void)luaext_timers_pause(sandbox, LUAEXT_TIMER_CPU | LUAEXT_TIMER_WALL);
		}

		/* One monotonic read, so the boundary below knows whether this call ran
		 * long enough to have plausibly crossed a deadline inside it. */
		call_started_ns = luaext_clock_monotonic_ns();

		luaext_timers_span_begin(sandbox, &sandbox->php_span_depth, &host_span);

		/*
		 * zend_call_known_fcc() rather than zend_call_function(): it copies a
		 * trampoline before calling, because zend_call_function() frees the one
		 * it is given, and the fcc it would be given here is the closure's own
		 * long-lived copy. The fn branch is the proxy path: a vetted
		 * zend_function on a known receiver needs no callable resolution at all.
		 */
		if (target->fcc != NULL) {
			zend_call_known_fcc(target->fcc, &result, (uint32_t)argc, params, NULL);
		} else {
			zend_call_known_function(target->fn, target->bound, target->scope, &result,
									 (uint32_t)argc, params, NULL);
		}

		luaext_timers_span_end(sandbox, &sandbox->php_span_depth, &host_span,
							   &sandbox->php_time_wall_ns, &sandbox->php_time_cpu_ns);

		sandbox->in_php--;

		/*
		 * A callback that paused its own billing and forgot to resume does not
		 * get to keep the pause. Whether the call was billed at all is
		 * Limits::$billHostTime (off by default, paused above); when it is on,
		 * only an explicit pauseTimers() un-bills a crossing, and only when
		 * every enclosing frame paused too.
		 *
		 * A zend_bailout inside the callback longjmps past this, leaving the
		 * pause outstanding as well as the in_php increment the same bailout
		 * already stranded. A leaked pause errs towards NOT billing, which is
		 * the one direction worth naming out loud.
		 *
		 * THERE IS DELIBERATELY NO zend_try AROUND THIS CALL, and the reason is
		 * not that the hazard was overlooked.
		 *
		 * Catching the bailout would let this frame put in_lua and in_php back.
		 * That is exactly the wrong thing to do. luaext_sandbox_close() uses
		 * `in_lua > 0` as its evidence that a call was abandoned mid-flight, and
		 * refuses to lua_close() a state in that condition -- so "restoring" the
		 * counter would erase the only signal that teardown has, and hand the
		 * collector a state whose C frames are gone. The bookkeeping being
		 * stranded is what makes the shutdown path safe.
		 *
		 * Nor is there anything else to recover. The watchdog slot is released
		 * by luaext_timers_detach() during close, which runs before that guard.
		 * A poisoned-sandbox flag would have nothing to protect: a bailout ends
		 * the request, so there is no later call to refuse.
		 *
		 * Measured rather than assumed, under a debug PHP with assertions on,
		 * across six bailout shapes -- PHP's memory_limit and
		 * max_execution_time, each with an open VFS handle, a suspended
		 * coroutine, a nested Lua->PHP->Lua chain, and pending finalisers. All
		 * end the request cleanly. exit() is a separate mechanism entirely and
		 * is handled in luaext_error.c; it is not a bailout at all.
		 */
		luaext_timers_php_returned(sandbox);
	}

	/*
	 * A thrown exception is the answer, so there is nothing to convert. Checked
	 * explicitly rather than inferred from the return value: an exception must
	 * never reach the interpreter as anything but a deliberate conversion.
	 */
	if (EG(exception) == NULL && converted && verdict == LUAEXT_PHPCALL_ARGS_OK) {
		lua_pushcfunction(L, luaext_phpcall_push_result);
		lua_pushlightuserdata(L, sandbox);
		lua_pushlightuserdata(L, &result);

		/*
		 * Lifted for exactly the length of the protected call. A raise inside it
		 * unwinds to this lua_pcall and no further, so it strands nothing this
		 * frame owns -- which is precisely what the assertion exists to check
		 * everywhere else.
		 */
		LUAEXT_NO_RAISE_END(L);
		status = lua_pcall(L, 2, 1, 0);
		LUAEXT_NO_RAISE_BEGIN(L);
	}

	if (Z_TYPE(result) != IS_UNDEF) {
		zval_ptr_dtor(&result);
		ZVAL_UNDEF(&result);
	}

	for (index = 0; index < argc; index++) {
		zval_ptr_dtor(&params[index]);
	}

	/* Unconditional, and separate from params_bytes: a conversion that failed
	 * part-way still charged for what it built, and argc==0 charges nothing but
	 * a refused first argument is not argc==0. */
	if (content_bytes > 0) {
		luaext_alloc_discharge(sandbox, content_bytes);
		content_bytes = 0;
	}

	if (params != NULL) {
		efree(params);
		params = NULL;
		luaext_alloc_discharge(sandbox, params_bytes);
	}

	LUAEXT_NO_RAISE_END(L);

	/* Nothing is owned from here down, so raising is finally safe. */

	/*
	 * The callback boundary, tier 3 of interrupt delivery. A limit that expired
	 * while the host was working gets delivered here rather than waiting for the
	 * script to execute another instruction -- which matters most for the
	 * callback that never returns to Lua at all because it is the last thing the
	 * script does.
	 *
	 * Sampled as well as flag-checked, for the same reason the return boundary
	 * is: the flag is set by the watchdog thread, whose wakeup can land after a
	 * breach inside this callback has already been crossed. No back edge runs
	 * inside PHP, so without the sample a breach here waits for the thread --
	 * and the return boundary, if the thread never wakes in time.
	 *
	 * Gated on the call having run for a millisecond, because the sample is a
	 * mutex and a clock syscall and this crossing is otherwise ~0.3us: sampling
	 * unconditionally measured 2.9x on a trivial callback. A callback shorter
	 * than the gate cannot have overshot by more than the gate, and a breach
	 * crossed inside one is still delivered at the next back edge or the return
	 * boundary -- the same bounded lateness every VM instruction already has.
	 * Long callbacks, the only ones that can meaningfully overshoot, pay one
	 * sample against work that dwarfs it.
	 */
	if (call_started_ns != 0) {
		/* Same regression guard as the span accounting: a failed clock read
		 * here only makes the sampling predicate spuriously true, but the two
		 * subtractions should not disagree about whether that can happen. */
		uint64_t now = luaext_clock_monotonic_ns();

		if (now >= call_started_ns && now - call_started_ns >= LUAEXT_PHPCALL_SAMPLE_AFTER_NS &&
			luaext_timers_final_check(sandbox)) {
			luaext_raise_interrupt(L);
		}
	}

	LUAEXT_CHECK(L);

	if (EG(exception) != NULL) {
		/*
		 * Retains the object and decides catchable versus fatal from its class.
		 * Does not return.
		 */
		luaext_error_raise_from_exception(L);
	}

	/* After the exception routing on purpose: a real exception (including one
	 * the callable check's autoload threw) outranks the gate's message. */
	if (verdict == LUAEXT_PHPCALL_ARGS_MISMATCH) {
		luaext_error_raise(L, LUAEXT_ERR_RUNTIME, false, "%s", gate_message);
	}

	if (status != LUA_OK) {
		/* The failed conversion left its error value on top; re-raise it
		 * unchanged so the host sees the ConversionError it describes. */
		return lua_error(L);
	}

	/*
	 * Unreachable while the conversion subsystem keeps its promise to throw on
	 * every failure. Returning here would hand the script the last argument as
	 * though it were the result, so the promise is checked rather than trusted.
	 */
	if (!converted) {
		luaext_error_raise(L, LUAEXT_ERR_CONVERSION, true,
						   "The arguments to the host callback %s could not be converted", label);
	}

	return 1;
}

/* The C closure every registered callable is reached through. */
static int luaext_phpcall_invoke(lua_State *L)
{
	luaext_phpcall_ud *slot = (luaext_phpcall_ud *)lua_touserdata(L, lua_upvalueindex(1));
	luaext_phpcall_target target;

	/* Only read out of the storage once the storage has been vouched for: a
	 * userdata that is not ours has no name field to read. */
	if (slot == NULL || slot->magic != LUAEXT_PHPCALL_MAGIC || !ZEND_FCC_INITIALIZED(slot->fcc)) {
		luaext_error_raise(L, LUAEXT_ERR_ABORT, true,
						   "A host callback was invoked after its storage was released");
	}

	target.fcc = &slot->fcc;
	target.fn = NULL;
	target.bound = NULL;
	target.scope = NULL;
	target.label = luaext_phpcall_label(slot);
	target.first_arg = 1;

	return luaext_phpcall_invoke_target(L, &target);
}

/* -------------------------------------------------------------------------
 * Exposing a callable
 * ---------------------------------------------------------------------- */

typedef struct {
	zend_fcall_info_cache fcc;
	const char *name;
	size_t name_len;
} luaext_phpcall_build;

/*
 * The allocating half of building a closure, run under lua_pcall.
 *
 * Every step here can raise on a memory error, and the callers are PHP method
 * bodies where a raise has nothing to unwind to -- it would reach lua_atpanic
 * and take the request with it. Protecting the whole build turns that into a
 * thrown exception.
 *
 * Argument 1: the build request.
 */
static int luaext_phpcall_build_closure(lua_State *L)
{
	luaext_phpcall_build *build = (luaext_phpcall_build *)lua_touserdata(L, 1);
	luaext_phpcall_ud *slot;

	lua_settop(L, 0);
	luaL_checkstack(L, LUAEXT_PHPCALL_SLOTS, "luaext: no stack to build a host callback");

	slot = (luaext_phpcall_ud *)lua_newuserdatauv(L, sizeof(*slot), 0);

	memset(slot, 0, sizeof(*slot));
	slot->magic = LUAEXT_PHPCALL_MAGIC;
	slot->fcc = empty_fcall_info_cache;

	/*
	 * __gc is armed before the storage owns anything. A raise from any step
	 * below then still leaves a userdata the collector will finalise, rather
	 * than one holding references nothing will ever release.
	 */
	luaext_phpcall_metatable(L);
	lua_setmetatable(L, -2);

	zend_fcc_dup(&slot->fcc, &build->fcc);

	if (build->name != NULL && build->name_len > 0) {
		slot->name = zend_string_init(build->name, build->name_len, 1);
	}

	lua_pushcclosure(L, luaext_phpcall_invoke, 1);

	return 1;
}

/*
 * Turn the Lua error a protected build failed with into a thrown PHP exception.
 *
 * Only a memory error can get here, so the class is the honest one rather than
 * a generic failure. lua_tostring() rather than luaL_tolstring(): this runs
 * outside any protected call, and a __tostring metamethod must not be given the
 * chance to raise where nothing would catch it.
 */
static void luaext_phpcall_throw_lua_failure(lua_State *L, const char *what, const char *name)
{
	const char *message = lua_tostring(L, -1);

	zend_throw_exception_ex(luaext_ce_memory_limit_error, 0, "Cannot %s \"%s\": %s", what,
							name != NULL ? name : LUAEXT_PHPCALL_ANONYMOUS,
							message != NULL ? message : "the interpreter ran out of memory");

	lua_pop(L, 1);
}

/* Reject a sandbox that cannot be registered into. */
static bool luaext_phpcall_usable(const luaext_sandbox *sandbox)
{
	if (sandbox == NULL || sandbox->closed || sandbox->L == NULL) {
		zend_throw_exception(luaext_ce_closed_sandbox_error, "The sandbox has been closed", 0);
		return false;
	}

	return true;
}

/*
 * Resolve `callable` into `fcc`, throwing a ConfigurationError naming `name` if
 * it is not callable.
 *
 * The engine's own wording is reused for the reason, so a host reads the same
 * explanation it would get from any other callable parameter.
 */
static bool luaext_phpcall_resolve(zval *callable, const char *name, zend_fcall_info_cache *fcc)
{
	char *reason = NULL;
	bool callable_ok;

	*fcc = empty_fcall_info_cache;
	callable_ok = zend_is_callable_ex(callable, NULL, 0, NULL, fcc, &reason);

	if (!callable_ok) {
		zend_throw_exception_ex(luaext_ce_configuration_error, 0,
								"Cannot expose \"%s\" to Lua: to be a valid callback, %s",
								name != NULL ? name : LUAEXT_PHPCALL_ANONYMOUS,
								reason != NULL ? reason : "it must be callable");
	}

	/* Set on some successful resolutions too, as a deprecation note. */
	if (reason != NULL) {
		efree(reason);
	}

	return callable_ok;
}

bool luaext_phpcall_push(luaext_sandbox *sandbox, lua_State *L, zval *callable, const char *name)
{
	luaext_phpcall_build build;

	if (!luaext_phpcall_usable(sandbox)) {
		return false;
	}

	if (callable == NULL) {
		zend_throw_exception(luaext_ce_configuration_error, "Cannot expose a missing value to Lua",
							 0);
		return false;
	}

	if (!luaext_phpcall_resolve(callable, name, &build.fcc)) {
		return false;
	}

	build.name = name;
	build.name_len = name != NULL ? strlen(name) : 0;

	if (!lua_checkstack(L, LUAEXT_PHPCALL_SLOTS)) {
		zend_throw_exception(
			luaext_ce_memory_limit_error,
			"Cannot expose a PHP callable to Lua: the interpreter stack cannot grow", 0);
		return false;
	}

	lua_pushcfunction(L, luaext_phpcall_build_closure);
	lua_pushlightuserdata(L, &build);

	if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
		luaext_phpcall_throw_lua_failure(L, "expose a PHP callable to Lua as", name);
		return false;
	}

	return true;
}

/* -------------------------------------------------------------------------
 * Exposing a table of callables
 * ---------------------------------------------------------------------- */

typedef struct {
	luaext_sandbox *sandbox;
	const char *name;
	size_t name_len;
	HashTable *functions;

	/* A PHP exception was thrown inside the protected build; the Lua side
	 * returned normally, so the status alone would not show it. */
	bool failed;
} luaext_phpcall_table;

/*
 * Build the library table and publish it, under lua_pcall.
 *
 * Argument 1: the registration request.
 */
static int luaext_phpcall_build_table(lua_State *L)
{
	luaext_phpcall_table *build = (luaext_phpcall_table *)lua_touserdata(L, 1);
	zend_string *key;
	zval *entry;

	lua_settop(L, 0);
	luaL_checkstack(L, LUAEXT_PHPCALL_SLOTS, "luaext: no stack to build a library table");

	/*
	 * Raw access throughout. The globals table may later carry a metatable that
	 * a script is not allowed to see through, and a host registering a library
	 * is not the caller that metatable exists to constrain.
	 */
	lua_pushglobaltable(L);
	lua_pushlstring(L, build->name, build->name_len);
	lua_rawget(L, -2);

	/* Adding to an existing table rather than replacing it. Registrations can
	 * no longer repeat a name — the claim table upstream refuses that — so
	 * the only table this can meet is one the host planted with setGlobal(),
	 * which stays the deliberate free-form write; anything that is not a
	 * table is replaced. */
	if (!lua_istable(L, -1)) {
		lua_pop(L, 1);
		lua_createtable(L, 0, (int)zend_hash_num_elements(build->functions));
	}

	ZEND_HASH_FOREACH_STR_KEY_VAL(build->functions, key, entry)
	{
		/* This frame's own state: the closure is rawset into the table below,
		 * so it has to land on the stack this function is building on. */
		if (!luaext_phpcall_push(build->sandbox, L, entry, ZSTR_VAL(key))) {
			/* The global is assigned last, so abandoning here leaves the
			 * interpreter without a half-built library in it. */
			build->failed = true;
			return 0;
		}

		lua_pushlstring(L, ZSTR_VAL(key), ZSTR_LEN(key));
		lua_insert(L, -2);
		lua_rawset(L, -3);
	}
	ZEND_HASH_FOREACH_END();

	lua_pushlstring(L, build->name, build->name_len);
	lua_insert(L, -2);
	lua_rawset(L, -3);

	return 0;
}

/*
 * Refuse a table of callables before a single Lua object is built for it, so a
 * rejected registration leaves the interpreter exactly as it was.
 */
static bool luaext_phpcall_check_functions(HashTable *functions)
{
	zend_string *key;
	zval *entry;

	if (functions == NULL || zend_hash_num_elements(functions) == 0) {
		zend_throw_exception(luaext_ce_configuration_error,
							 "A Lua library must expose at least one callable", 0);
		return false;
	}

	ZEND_HASH_FOREACH_STR_KEY_VAL(functions, key, entry)
	{
		if (key == NULL || ZSTR_LEN(key) == 0) {
			zend_throw_exception(
				luaext_ce_configuration_error,
				"Every entry of a Lua library must be keyed by the non-empty name Lua will see", 0);
			return false;
		}

		if (!zend_is_callable(entry, 0, NULL)) {
			zend_throw_exception_ex(luaext_ce_configuration_error, 0,
									"The Lua library entry \"%s\" is not a valid callback",
									ZSTR_VAL(key));
			return false;
		}
	}
	ZEND_HASH_FOREACH_END();

	return true;
}

bool luaext_phpcall_register_table(luaext_sandbox *sandbox, const char *name, size_t name_len,
								   HashTable *functions)
{
	luaext_phpcall_table build;
	lua_State *L;

	if (!luaext_phpcall_usable(sandbox)) {
		return false;
	}

	if (name == NULL || name_len == 0) {
		zend_throw_exception(luaext_ce_configuration_error,
							 "A Lua library needs a name for scripts to reach it by", 0);
		return false;
	}

	if (!luaext_phpcall_check_functions(functions)) {
		return false;
	}

	build.sandbox = sandbox;
	build.name = name;
	build.name_len = name_len;
	build.functions = functions;
	build.failed = false;

	/* The running state, not the main thread: a registration made from inside
	 * a coroutine's host callback would otherwise run this pcall on a stack
	 * whose C-call budget lua_resume lent to the coroutine. */
	L = luaext_exec_state(sandbox);

	if (!lua_checkstack(L, LUAEXT_PHPCALL_SLOTS)) {
		zend_throw_exception(luaext_ce_memory_limit_error,
							 "Cannot register a Lua library: the interpreter stack cannot grow", 0);
		return false;
	}

	lua_pushcfunction(L, luaext_phpcall_build_table);
	lua_pushlightuserdata(L, &build);

	if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
		luaext_phpcall_throw_lua_failure(L, "register the Lua library", name);
		return false;
	}

	return !build.failed;
}

/* -------------------------------------------------------------------------
 * Selecting an object's methods
 *
 * Explicit only, in both directions. A host that adds a public method to a
 * class it happens to have registered must not thereby widen what untrusted
 * code may call, which is why there is no "expose everything public" mode and
 * why a class with neither an allowlist nor an attribute is an error rather
 * than an empty table.
 * ---------------------------------------------------------------------- */

/* Why a method cannot be exposed, or NULL when it can. */
static const char *luaext_phpcall_method_refusal(const zend_function *method)
{
	uint32_t flags = method->common.fn_flags;
	const zend_string *method_name = method->common.function_name;

	if ((flags & ZEND_ACC_PUBLIC) == 0) {
		return "it is not public";
	}

	if ((flags & ZEND_ACC_STATIC) != 0) {
		return "it is static, and only bound instance methods cross this boundary";
	}

	if ((flags & ZEND_ACC_ABSTRACT) != 0) {
		return "it is abstract";
	}

	/*
	 * Magic methods are refused wholesale, and __call is the reason. Exposing it
	 * would turn one entry in an allowlist into every name the class can be
	 * asked for, which is exactly the implicit surface this bridge exists to
	 * avoid. The others -- __get, __destruct, __toString -- are no better as
	 * script-callable entry points.
	 */
	if (method_name != NULL && ZSTR_LEN(method_name) >= 2 && ZSTR_VAL(method_name)[0] == '_' &&
		ZSTR_VAL(method_name)[1] == '_') {
		return "it is a magic method, and magic methods are never exposed";
	}

	return NULL;
}

/*
 * Add `method`, bound to `instance`, under the name Lua will see.
 *
 * The callable is an array pair rather than a closure: the pair is what the
 * header promises, it keeps the instance alive by ordinary refcounting, and it
 * resolves to the exact zend_function this selection already vetted.
 */
static bool luaext_phpcall_add_method(HashTable *methods, zval *instance, zend_function *method,
									  zend_string *lua_name)
{
	const zend_class_entry *scope = method->common.scope;
	zval callable;
	zval bound;

	array_init_size(&callable, 2);

	ZVAL_COPY(&bound, instance);
	add_next_index_zval(&callable, &bound);
	add_next_index_str(&callable, zend_string_copy(method->common.function_name));

	if (zend_hash_add(methods, lua_name, &callable) == NULL) {
		zval_ptr_dtor(&callable);
		zend_throw_exception_ex(
			luaext_ce_configuration_error, 0, "Two methods of %s both want the Lua name \"%s\"",
			scope != NULL ? ZSTR_VAL(scope->name) : "the object", ZSTR_VAL(lua_name));
		return false;
	}

	return true;
}

/*
 * The name #[LuaMethod] asks for, or the method's own name.
 *
 * The attribute is instantiated rather than read out of its argument list, so
 * `#[LuaMethod('query')]` and `#[LuaMethod(name: 'query')]` and a constant
 * expression all behave the way the host wrote them.
 *
 * Returns a reference the caller releases, or false with an exception thrown.
 */
bool luaext_phpcall_attribute_name(zend_attribute *attribute, zend_function *method,
								   zend_string **out)
{
	zend_class_entry *scope = method->common.scope;
	zend_string *filename = NULL;
	zval marker;
	zval holder;
	zval *configured;

	if (scope != NULL && scope->type == ZEND_USER_CLASS) {
		filename = scope->info.user.filename;
	}

	if (zend_get_attribute_object(&marker, luaext_ce_lua_method_attribute, attribute, scope,
								  filename) != SUCCESS) {
		return false;
	}

	configured = zend_read_property(luaext_ce_lua_method_attribute, Z_OBJ(marker),
									ZEND_STRL("name"), true, &holder);

	if (configured != NULL && Z_TYPE_P(configured) == IS_STRING) {
		if (Z_STRLEN_P(configured) == 0) {
			zval_ptr_dtor(&marker);
			zend_throw_exception_ex(luaext_ce_configuration_error, 0,
									"%s::%s() carries a #[LuaMethod] with an empty name",
									scope != NULL ? ZSTR_VAL(scope->name) : "?",
									ZSTR_VAL(method->common.function_name));
			return false;
		}

		/* Copied before the marker is released: it owns the string. */
		*out = zend_string_copy(Z_STR_P(configured));
	} else {
		*out = zend_string_copy(method->common.function_name);
	}

	zval_ptr_dtor(&marker);

	return true;
}

/* The caller's explicit allowlist, which overrides every attribute. */
static bool luaext_phpcall_collect_allowlist(HashTable *methods, zval *instance,
											 zend_class_entry *ce, HashTable *allowlist)
{
	zval *entry;

	ZEND_HASH_FOREACH_VAL(allowlist, entry)
	{
		zend_string *requested;
		zend_function *method;
		const char *refusal;

		ZVAL_DEREF(entry);

		if (Z_TYPE_P(entry) != IS_STRING || Z_STRLEN_P(entry) == 0) {
			zend_throw_exception_ex(luaext_ce_configuration_error, 0,
									"The method allowlist for %s must hold non-empty method names",
									ZSTR_VAL(ce->name));
			return false;
		}

		requested = Z_STR_P(entry);

		/*
		 * Looked up in the class's own table rather than resolved as a callable:
		 * a name the class does not declare must be an error, not a silent
		 * detour through __call.
		 *
		 * _lc() because a function_table is keyed lowercase, and because it
		 * hashes case-insensitively off the bytes rather than building a lowered
		 * copy to throw away. The copy is where luaext_vfs.c's 48-per-test leak
		 * came from -- this site released correctly, but the shape is the one
		 * that goes wrong, and tools/check-banned-idioms.sh now refuses it.
		 */
		method = (zend_function *)zend_hash_str_find_ptr_lc(
			&ce->function_table, ZSTR_VAL(requested), ZSTR_LEN(requested));

		if (method == NULL) {
			zend_throw_exception_ex(luaext_ce_configuration_error, 0,
									"%s has no method %s() to expose to Lua", ZSTR_VAL(ce->name),
									ZSTR_VAL(requested));
			return false;
		}

		refusal = luaext_phpcall_method_refusal(method);

		if (refusal != NULL) {
			zend_throw_exception_ex(luaext_ce_configuration_error, 0,
									"%s::%s() cannot be exposed to Lua: %s", ZSTR_VAL(ce->name),
									ZSTR_VAL(requested), refusal);
			return false;
		}

		/* The name as the host wrote it, so an allowlist reads the same on both
		 * sides of the boundary. */
		if (!luaext_phpcall_add_method(methods, instance, method, requested)) {
			return false;
		}
	}
	ZEND_HASH_FOREACH_END();

	if (zend_hash_num_elements(methods) == 0) {
		zend_throw_exception_ex(luaext_ce_configuration_error, 0,
								"The method allowlist for %s selects no method, so there would be "
								"nothing for a script to call",
								ZSTR_VAL(ce->name));
		return false;
	}

	return true;
}

/* Every method carrying #[LuaMethod], honouring the name it asks for. */
static bool luaext_phpcall_collect_attributed(HashTable *methods, zval *instance,
											  zend_class_entry *ce)
{
	zend_string *marker_name = zend_string_tolower(luaext_ce_lua_method_attribute->name);
	zend_function *method;
	bool collected = true;

	ZEND_HASH_MAP_FOREACH_PTR(&ce->function_table, method)
	{
		zend_attribute *attribute = zend_get_attribute(method->common.attributes, marker_name);
		zend_string *lua_name;
		const char *refusal;

		if (attribute == NULL) {
			continue;
		}

		refusal = luaext_phpcall_method_refusal(method);

		if (refusal != NULL) {
			/* Marked but unexposable is a host mistake, and a silently missing
			 * function is a worse way to find out about it than an exception. */
			zend_throw_exception_ex(
				luaext_ce_configuration_error, 0,
				"%s::%s() carries #[LuaMethod] but cannot be exposed to Lua: %s",
				ZSTR_VAL(ce->name), ZSTR_VAL(method->common.function_name), refusal);
			collected = false;
			break;
		}

		if (!luaext_phpcall_attribute_name(attribute, method, &lua_name)) {
			collected = false;
			break;
		}

		collected = luaext_phpcall_add_method(methods, instance, method, lua_name);
		zend_string_release(lua_name);

		if (!collected) {
			break;
		}
	}
	ZEND_HASH_FOREACH_END();

	zend_string_release(marker_name);

	if (!collected) {
		return false;
	}

	if (zend_hash_num_elements(methods) == 0) {
		zend_throw_exception_ex(
			luaext_ce_configuration_error, 0,
			"No method of %s carries #[LuaMethod] and no allowlist was given, so nothing would be "
			"exposed; selection is always explicit",
			ZSTR_VAL(ce->name));
		return false;
	}

	return true;
}

HashTable *luaext_phpcall_collect_methods(zval *instance, HashTable *allowlist)
{
	zend_class_entry *ce;
	HashTable *methods;
	bool collected;

	if (instance == NULL || Z_TYPE_P(instance) != IS_OBJECT) {
		zend_throw_exception(luaext_ce_configuration_error,
							 "Only an object's methods can be exposed to Lua", 0);
		return NULL;
	}

	ce = Z_OBJCE_P(instance);
	methods = zend_new_array(allowlist != NULL ? zend_hash_num_elements(allowlist) : 8);

	if (allowlist != NULL) {
		collected = luaext_phpcall_collect_allowlist(methods, instance, ce, allowlist);
	} else {
		collected = luaext_phpcall_collect_attributed(methods, instance, ce);
	}

	if (!collected) {
		zend_array_destroy(methods);
		return NULL;
	}

	return methods;
}
