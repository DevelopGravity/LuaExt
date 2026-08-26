/*
 * luaext — require(). See luaext_require.h for the six steps and why.
 */

#include "luaext_require.h"

#include "luaext_error.h"
#include "luaext_vfs.h"
#include "luaext_vfs_path.h"

#include <lauxlib.h>
#include <lua.h>

#include <Zend/zend_exceptions.h>
#include <Zend/zend_interfaces.h>
#include <Zend/zend_object_handlers.h>

#include <string.h>

/* -------------------------------------------------------------------------
 * Configuration
 * ---------------------------------------------------------------------- */

static zval *luaext_require_property(zval *config, const char *name)
{
	if (config == NULL || Z_TYPE_P(config) != IS_OBJECT) {
		return NULL;
	}

	return zend_read_property(luaext_ce_sandbox_config, Z_OBJ_P(config), name, strlen(name), 1,
							  NULL);
}

bool luaext_require_init_from_config(luaext_sandbox *sandbox, zval *config)
{
	zval *resolver = luaext_require_property(config, "moduleResolver");
	zval *paths = luaext_require_property(config, "modulePaths");

	ZVAL_UNDEF(&sandbox->module_resolver_zv);
	ZVAL_UNDEF(&sandbox->module_paths_zv);
	sandbox->require_depth = 0;

	if (resolver != NULL && Z_TYPE_P(resolver) == IS_OBJECT) {
		ZVAL_COPY(&sandbox->module_resolver_zv, resolver);
	}

	if (paths != NULL && Z_TYPE_P(paths) == IS_ARRAY) {
		ZVAL_COPY(&sandbox->module_paths_zv, paths);
	}

	return true;
}

void luaext_require_shutdown(luaext_sandbox *sandbox)
{
	if (sandbox == NULL) {
		return;
	}

	if (Z_TYPE(sandbox->module_resolver_zv) == IS_OBJECT) {
		zval_ptr_dtor(&sandbox->module_resolver_zv);
	}

	if (Z_TYPE(sandbox->module_paths_zv) == IS_ARRAY) {
		zval_ptr_dtor(&sandbox->module_paths_zv);
	}

	ZVAL_UNDEF(&sandbox->module_resolver_zv);
	ZVAL_UNDEF(&sandbox->module_paths_zv);
	sandbox->require_depth = 0;
}

/* -------------------------------------------------------------------------
 * Names
 * ---------------------------------------------------------------------- */

/*
 * Whether `name` is one require() will look for.
 *
 * A module name is substituted into a search pattern and becomes a VFS path, so
 * anything permitted here is something the host's backend can be asked for. The
 * grammar is therefore deliberately narrower than "a Lua string": letters,
 * digits, underscore, dot and hyphen, and no ".." anywhere -- a dot is legal
 * because module names conventionally use them as separators, and it is exactly
 * that convention which makes ".." reachable if it is not refused here.
 */
static bool luaext_require_name_ok(const char *name, size_t len)
{
	size_t index;

	if (len == 0 || len > LUAEXT_REQUIRE_MAX_NAME) {
		return false;
	}

	for (index = 0; index < len; index++) {
		char c = name[index];

		if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
			  c == '_' || c == '.' || c == '-')) {
			return false;
		}

		/* Refused wherever it appears, not merely as a whole component: the name
		 * is pasted into a pattern, so "a..b" would reach the canonicaliser as a
		 * traversal it then has to argue about. Better it never forms one. */
		if (c == '.' && index + 1 < len && name[index + 1] == '.') {
			return false;
		}
	}

	return true;
}

/* -------------------------------------------------------------------------
 * The registry tables
 * ---------------------------------------------------------------------- */

static void luaext_require_push_table(lua_State *L, const char *key)
{
	if (lua_rawgetp(L, LUA_REGISTRYINDEX, key) == LUA_TTABLE) {
		return;
	}

	lua_pop(L, 1);
	lua_createtable(L, 0, 8);
	lua_pushvalue(L, -1);
	lua_rawsetp(L, LUA_REGISTRYINDEX, key);
}

/* -------------------------------------------------------------------------
 * Loading resolved source
 * ---------------------------------------------------------------------- */

/*
 * Compile `code` and leave the chunk on the stack.
 *
 * Text mode unless the host both granted loadBytecode AND the source says it is
 * bytecode. Lua has no bytecode verifier, so "bt" here is the same boundary
 * compile() guards: a resolver that can hand back a binary chunk can hand back
 * native execution, which is why the capability gates it rather than the source
 * flag alone.
 */
static bool luaext_require_load(lua_State *L, luaext_sandbox *sandbox, const char *code,
								size_t code_len, const char *chunk_name, bool is_bytecode)
{
	size_t max_source = sandbox->policy.limits.max_source_bytes;
	bool allow_binary = is_bytecode && luaext_has_cap(&sandbox->policy, LUAEXT_CAP_LOAD_BYTECODE);
	int status;

	if (is_bytecode && !allow_binary) {
		luaext_error_raise(L, LUAEXT_ERR_MODULE, false, "%s",
						   "That module is bytecode, and this sandbox was not granted the "
						   "loadBytecode capability");
		return false;
	}

	/*
	 * The same ceiling compile() applies, and for the same reason: the parser
	 * runs before any interrupt can land, so a pathological source is bounded by
	 * its length or not at all.
	 */
	if (max_source != 0 && code_len > max_source) {
		luaext_error_raise(L, LUAEXT_ERR_MODULE, false,
						   "That module is %zu bytes, which exceeds the %zu byte source limit",
						   code_len, max_source);
		return false;
	}

	status = luaL_loadbufferx(L, code, code_len, chunk_name, allow_binary ? "bt" : "t");

	if (status != LUA_OK) {
		/* The loader's message is on the stack; re-raised as a module error so
		 * the caller learns which module rather than only what the parser said. */
		const char *message = lua_tostring(L, -1);

		luaext_error_raise(L, LUAEXT_ERR_MODULE, false, "%s",
						   message != NULL ? message : "the module did not compile");
		return false;
	}

	return true;
}

/* -------------------------------------------------------------------------
 * Step 5 -- the VFS, along modulePaths
 * ---------------------------------------------------------------------- */

/*
 * Substitute `name` for each '?' in `pattern`.
 *
 * Bounded before it is built rather than after: the pattern is host-supplied and
 * the name is script-supplied, so the product of a long pattern and many '?'
 * marks is the one length here a script has any influence over.
 */
static zend_string *luaext_require_expand(const char *pattern, size_t pattern_len, const char *name,
										  size_t name_len)
{
	smart_str out = {0};
	size_t index;
	size_t marks = 0;

	for (index = 0; index < pattern_len; index++) {
		if (pattern[index] == '?') {
			marks++;
		}
	}

	if (pattern_len + marks * name_len > LUAEXT_VFS_PATH_MAX_INPUT) {
		return NULL;
	}

	for (index = 0; index < pattern_len; index++) {
		if (pattern[index] == '?') {
			smart_str_appendl(&out, name, name_len);
		} else {
			smart_str_appendc(&out, pattern[index]);
		}
	}

	smart_str_0(&out);

	return out.s;
}

/*
 * Try each search pattern in turn, pushing the compiled chunk on the first hit.
 *
 * Returns 1 on a hit, 0 when nothing matched (which is not an error -- the
 * resolver is asked next), and -1 when something failed with a Lua error raised
 * or a PHP exception pending.
 */
static int luaext_require_search_vfs(lua_State *L, luaext_sandbox *sandbox, const char *name,
									 size_t name_len)
{
	zval *entry;

	if (!luaext_vfs_available(sandbox) || Z_TYPE(sandbox->module_paths_zv) != IS_ARRAY) {
		return 0;
	}

	ZEND_HASH_FOREACH_VAL(Z_ARRVAL(sandbox->module_paths_zv), entry)
	{
		zend_string *expanded;
		char *canonical;
		size_t canonical_len = 0;
		luaext_vfs_path_status status;
		zval args[1];
		zval result;
		zend_string *refusal = NULL;
		luaext_vfs_result outcome;
		bool exists;

		if (Z_TYPE_P(entry) != IS_STRING) {
			continue;
		}

		expanded = luaext_require_expand(Z_STRVAL_P(entry), Z_STRLEN_P(entry), name, name_len);

		if (expanded == NULL) {
			continue;
		}

		canonical = emalloc(ZSTR_LEN(expanded) + 2);
		status = luaext_vfs_path_canonical(ZSTR_VAL(expanded), ZSTR_LEN(expanded), canonical,
										   ZSTR_LEN(expanded) + 2, &canonical_len,
										   sandbox->policy.vfs_quota.max_path_length,
										   sandbox->policy.vfs_quota.max_path_depth);
		zend_string_release(expanded);

		if (status != LUAEXT_VFS_PATH_OK) {
			/* A pattern that cannot be canonicalised is skipped rather than
			 * fatal: the host wrote it, the script only supplied a name, and one
			 * unusable entry should not stop the rest of the list being tried. */
			efree(canonical);
			continue;
		}

		ZVAL_STR(&args[0], zend_string_init(canonical, canonical_len, 0));
		efree(canonical);

		outcome = luaext_vfs_call(L, sandbox, "exists", 1, args, &result, &refusal);

		if (outcome != LUAEXT_VFS_OK) {
			zval_ptr_dtor(&args[0]);

			if (refusal != NULL) {
				/* The backend declined to answer for this path. Treated as "not
				 * here" so the search continues, which is what a missing file
				 * means anyway. */
				zend_string_release(refusal);
				continue;
			}

			return -1;
		}

		exists = Z_TYPE(result) == IS_TRUE;
		zval_ptr_dtor(&result);

		if (!exists) {
			zval_ptr_dtor(&args[0]);
			continue;
		}

		outcome = luaext_vfs_call(L, sandbox, "read", 1, args, &result, &refusal);

		if (outcome != LUAEXT_VFS_OK) {
			zval_ptr_dtor(&args[0]);

			if (refusal != NULL) {
				zend_string_release(refusal);
				continue;
			}

			return -1;
		}

		if (Z_TYPE(result) != IS_STRING) {
			zval_ptr_dtor(&args[0]);
			zval_ptr_dtor(&result);
			luaext_error_raise(L, LUAEXT_ERR_MODULE, false, "%s",
							   "FileSystem::read() did not return a string for a module");
			return -1;
		}

		{
			/*
			 * The source and the chunk name are moved into LUA-owned memory, and
			 * every PHP-side allocation is released, before anything that can
			 * raise runs.
			 *
			 * Ordering alone is not enough here, which is the trap worth
			 * recording: luaext_require_load() raises on a source that does not
			 * compile, and a raise is a longjmp, so a zend_string still held --
			 * even one held only to pass to the very call that raises -- is
			 * simply lost. Lua strings survive that, because unwinding drops
			 * them and the collector takes them.
			 *
			 * '@' is Lua's convention for "this chunk came from a file", and is
			 * what makes a traceback name the module rather than quote it.
			 */
			size_t source_len;
			const char *source;
			const char *chunk_name;
			bool loaded;

			lua_pushlstring(L, Z_STRVAL(result), Z_STRLEN(result));
			lua_pushfstring(L, "@%s", Z_STRVAL(args[0]));

			zval_ptr_dtor(&args[0]);
			zval_ptr_dtor(&result);

			source = lua_tolstring(L, -2, &source_len);
			chunk_name = lua_tostring(L, -1);

			loaded = luaext_require_load(L, sandbox, source, source_len, chunk_name, false);

			if (!loaded) {
				return -1;
			}

			/* [source, name, chunk] -> [chunk] */
			lua_remove(L, -3);
			lua_remove(L, -2);

			return 1;
		}
	}
	ZEND_HASH_FOREACH_END();

	return 0;
}

/* -------------------------------------------------------------------------
 * Step 6 -- the host resolver
 * ---------------------------------------------------------------------- */

static int luaext_require_ask_resolver(lua_State *L, luaext_sandbox *sandbox, const char *name,
									   size_t name_len, const char *requested_by)
{
	zend_string *method;
	zend_function *fn;
	zval args[2];
	zval result;
	zval *code;
	zval *chunk_name;
	zval *is_bytecode;
	bool loaded;

	if (Z_TYPE(sandbox->module_resolver_zv) != IS_OBJECT) {
		return 0;
	}

	method = zend_string_init("resolve", strlen("resolve"), 0);
	fn = zend_hash_find_ptr(&Z_OBJCE(sandbox->module_resolver_zv)->function_table, method);
	zend_string_release(method);

	if (fn == NULL) {
		luaext_error_raise(L, LUAEXT_ERR_MODULE, false, "%s",
						   "The configured ModuleResolver does not implement resolve()");
		return -1;
	}

	/* Holds zvals from here. The END is placed at each exit, and immediately
	 * before luaext_require_load(), which raises by design. */
	LUAEXT_NO_RAISE_BEGIN(L);

	ZVAL_STR(&args[0], zend_string_init(name, name_len, 0));
	ZVAL_STR(&args[1], zend_string_init(requested_by, strlen(requested_by), 0));

	ZVAL_UNDEF(&result);
	zend_call_known_instance_method(fn, Z_OBJ(sandbox->module_resolver_zv), &result, 2, args);

	zval_ptr_dtor(&args[0]);
	zval_ptr_dtor(&args[1]);

	if (EG(exception)) {
		/*
		 * Left pending, exactly as a FileSystem's non-VfsError is. A resolver
		 * that throws is the host failing, and converting it into "module not
		 * found" would hide a real fault behind a script-visible condition.
		 */
		zval_ptr_dtor(&result);
		LUAEXT_NO_RAISE_END(L);
		return -1;
	}

	if (Z_TYPE(result) != IS_OBJECT) {
		/* Null is the documented "I do not provide this one". */
		zval_ptr_dtor(&result);
		LUAEXT_NO_RAISE_END(L);
		return 0;
	}

	code =
		zend_read_property(luaext_ce_module_source, Z_OBJ(result), "code", strlen("code"), 1, NULL);
	chunk_name = zend_read_property(luaext_ce_module_source, Z_OBJ(result), "chunkName",
									strlen("chunkName"), 1, NULL);
	is_bytecode = zend_read_property(luaext_ce_module_source, Z_OBJ(result), "isBytecode",
									 strlen("isBytecode"), 1, NULL);

	if (code == NULL || Z_TYPE_P(code) != IS_STRING || chunk_name == NULL ||
		Z_TYPE_P(chunk_name) != IS_STRING) {
		zval_ptr_dtor(&result);
		LUAEXT_NO_RAISE_END(L);
		luaext_error_raise(L, LUAEXT_ERR_MODULE, false, "%s",
						   "ModuleResolver::resolve() returned something that is not a "
						   "ModuleSource");
		return -1;
	}

	loaded =
		luaext_require_load(L, sandbox, Z_STRVAL_P(code), Z_STRLEN_P(code), Z_STRVAL_P(chunk_name),
							is_bytecode != NULL && Z_TYPE_P(is_bytecode) == IS_TRUE);

	zval_ptr_dtor(&result);

	return loaded ? 1 : -1;
}

/* -------------------------------------------------------------------------
 * require
 * ---------------------------------------------------------------------- */

static int luaext_require_call(lua_State *L)
{
	luaext_sandbox *sandbox = LUAEXT_SB(L);
	size_t name_len;
	const char *name = luaL_checklstring(L, 1, &name_len);
	uint32_t max_modules = sandbox->policy.limits.max_modules;
	uint32_t max_depth = sandbox->policy.limits.max_require_depth;
	const char *requested_by;
	int found;
	int status;

	if (!luaext_require_name_ok(name, name_len)) {
		luaext_error_raise(L, LUAEXT_ERR_MODULE, false,
						   "\"%s\" is not a usable module name: letters, digits, '_', '.' and "
						   "'-' only, at most %u bytes, and no \"..\"",
						   name, (unsigned int)LUAEXT_REQUIRE_MAX_NAME);
	}

	/* 1. Already loaded. */
	luaext_require_push_table(L, &luaext_key_loaded);
	lua_pushlstring(L, name, name_len);

	if (lua_rawget(L, -2) != LUA_TNIL) {
		return 1;
	}

	lua_pop(L, 2);

	/*
	 * 2. The circular guard, kept in its own table rather than by writing a
	 * sentinel into package.loaded. Sharing one table is what makes "in progress"
	 * and "loaded" indistinguishable, and it is why upstream leaves a failed
	 * module cached.
	 */
	luaext_require_push_table(L, &luaext_key_loading);
	lua_pushlstring(L, name, name_len);

	if (lua_rawget(L, -2) != LUA_TNIL) {
		luaext_error_raise(L, LUAEXT_ERR_MODULE, false,
						   "Module \"%s\" requires itself, directly or through another module",
						   name);
	}

	lua_pop(L, 1);

	/* 3. The two limits. */
	if (max_depth != 0 && sandbox->require_depth >= max_depth) {
		luaext_error_raise(L, LUAEXT_ERR_MODULE, true,
						   "Loading \"%s\" would nest require() %u deep, which is the sandbox's "
						   "Limits::$maxRequireDepth",
						   name, (unsigned int)max_depth + 1);
	}

	if (max_modules != 0 && sandbox->modules_loaded >= (uint64_t)max_modules) {
		luaext_error_raise(L, LUAEXT_ERR_MODULE, true,
						   "The sandbox has already loaded %u module(s), which is its "
						   "Limits::$maxModules",
						   (unsigned int)max_modules);
	}

	/* Marked in progress before any loader runs, so a module that requires
	 * itself while loading meets the guard above rather than recursing. */
	lua_pushlstring(L, name, name_len);
	lua_pushboolean(L, 1);
	lua_rawset(L, -3);
	lua_pop(L, 1);

	sandbox->require_depth++;

	requested_by = lua_tostring(L, lua_upvalueindex(1));

	if (requested_by == NULL) {
		requested_by = "=main";
	}

	/* 4. package.preload. */
	luaext_require_push_table(L, &luaext_key_preload);
	lua_pushlstring(L, name, name_len);

	if (lua_rawget(L, -2) != LUA_TNIL) {
		lua_remove(L, -2); /* the preload table; the loader stays */
		found = 1;
	} else {
		lua_pop(L, 2);

		/* 5. The VFS. */
		found = luaext_require_search_vfs(L, sandbox, name, name_len);

		/* 6. The resolver. */
		if (found == 0) {
			found = luaext_require_ask_resolver(L, sandbox, name, name_len, requested_by);
		}
	}

	if (found <= 0) {
		/*
		 * Unmarked before failing, or a module that merely could not be found
		 * once would be permanently unrequirable -- the guard would report it as
		 * circular on the next attempt.
		 */
		sandbox->require_depth--;
		luaext_require_push_table(L, &luaext_key_loading);
		lua_pushlstring(L, name, name_len);
		lua_pushnil(L);
		lua_rawset(L, -3);
		lua_pop(L, 1);

		if (found < 0) {
			/* A raised Lua error or a pending PHP exception; propagate it. */
			if (EG(exception) != NULL) {
				luaext_error_raise_from_exception(L);
			}

			return lua_error(L);
		}

		luaext_error_raise(L, LUAEXT_ERR_MODULE, false, "Module \"%s\" was not found", name);
	}

	/*
	 * Protected, and that is what makes "a failed module is not cached" true.
	 *
	 * An unprotected lua_call propagates straight past this frame, so the
	 * in-progress mark set above would survive the failure -- and the NEXT
	 * require of that name would meet its own leftover mark and be reported as
	 * circular. The retry the docs promise would be impossible, and the
	 * diagnosis would blame the wrong thing entirely.
	 */
	lua_pushlstring(L, name, name_len);
	status = lua_pcall(L, 1, 1, 0);

	sandbox->require_depth--;

	/* Unmarked on BOTH paths, before the error is re-raised. */
	luaext_require_push_table(L, &luaext_key_loading);
	lua_pushlstring(L, name, name_len);
	lua_pushnil(L);
	lua_rawset(L, -3);
	lua_pop(L, 1);

	if (status != LUA_OK) {
		/*
		 * Re-raised rather than reported, and keyed on the STATUS for the reason
		 * coroutine.resume is: LUA_ERRMEM carries Lua's own preallocated string
		 * instead of our unforgeable marker, so re-raising it as-is would leave
		 * an enclosing pcall seeing an ordinary catchable error and a script
		 * could require its way past memoryBytes. Every other status re-raises
		 * the original value, which keeps a fatal fatal and a catchable error
		 * catchable.
		 */
		if (status == LUA_ERRMEM) {
			lua_pop(L, 1);
			luaext_error_raise(L, LUAEXT_ERR_MEMORY, true,
							   "The sandbox is out of memory; a script may not catch its own "
							   "memory limit being reached");
		}

		return lua_error(L);
	}

	/* A loader returning nothing means the module is `true`, as upstream does. */
	if (lua_isnil(L, -1)) {
		lua_pop(L, 1);
		lua_pushboolean(L, 1);
	}

	luaext_require_push_table(L, &luaext_key_loaded);
	lua_pushlstring(L, name, name_len);
	lua_pushvalue(L, -3);
	lua_rawset(L, -3);
	lua_pop(L, 1);

	sandbox->modules_loaded++;

	return 1;
}

/* -------------------------------------------------------------------------
 * preloadModule
 * ---------------------------------------------------------------------- */

/* Arguments: the loader, then the module name. Runs under the protected call
 * below, so a memory error building the table unwinds there. */
static int luaext_require_preload_store(lua_State *L)
{
	luaext_require_push_table(L, &luaext_key_preload);
	lua_pushvalue(L, 2); /* name */
	lua_pushvalue(L, 1); /* loader */
	lua_rawset(L, -3);

	return 0;
}

bool luaext_require_preload(lua_State *L, luaext_sandbox *sandbox, const char *name,
							size_t name_len)
{
	(void)sandbox;

	if (!luaext_require_name_ok(name, name_len)) {
		lua_pop(L, 1);
		zend_throw_exception_ex(luaext_ce_configuration_error, 0,
								"\"%s\" is not a usable module name: letters, digits, '_', '.' "
								"and '-' only, at most %u bytes, and no \"..\"",
								name, (unsigned int)LUAEXT_REQUIRE_MAX_NAME);
		return false;
	}

	if (!lua_checkstack(L, 4)) {
		lua_pop(L, 1);
		zend_throw_exception(luaext_ce_memory_limit_error,
							 "Cannot preload a module: the interpreter stack cannot grow", 0);
		return false;
	}

	/*
	 * PROTECTED, and that is the point rather than ceremony.
	 *
	 * This runs from a PHP method, so there is no enclosing lua_pcall to catch
	 * anything it raises -- and building the preload table allocates, which means
	 * a memory error here would reach the panic function. A panic cannot return
	 * (Lua calls abort() if it does), so the handler has to end the request, and
	 * it does so by longjmping straight past lua_close: the whole Lua heap, which
	 * lives outside PHP's allocator, is leaked.
	 *
	 * luaext_phpcall_push() protects its own pushes for exactly this reason. This
	 * is the same shape.
	 */
	lua_pushcfunction(L, luaext_require_preload_store);
	lua_insert(L, -2); /* [store, loader] */
	lua_pushlstring(L, name, name_len);

	if (lua_pcall(L, 2, 0, 0) != LUA_OK) {
		const char *message = lua_tostring(L, -1);

		zend_throw_exception_ex(luaext_ce_memory_limit_error, 0, "Cannot preload \"%s\": %s", name,
								message != NULL ? message : "the interpreter ran out of memory");
		lua_pop(L, 1);

		return false;
	}

	return true;
}

/* -------------------------------------------------------------------------
 * Install
 * ---------------------------------------------------------------------- */

bool luaext_require_install(lua_State *L, luaext_sandbox *sandbox)
{
	if (!luaext_has_cap(&sandbox->policy, LUAEXT_CAP_REQUIRE)) {
		return true;
	}

	luaL_checkstack(L, 8, "luaext: no stack to build the package library");

	/*
	 * require carries the requiring chunk's name as an upvalue so a resolver can
	 * be told who asked. The main chunk has no module name, so it is the
	 * default, and each module's own require sees its own name -- which is what
	 * makes relative resolution possible for a host that wants it.
	 */
	lua_pushliteral(L, "=main");
	lua_pushcclosure(L, luaext_require_call, 1);
	lua_setglobal(L, "require");

	lua_createtable(L, 0, 3);

	luaext_require_push_table(L, &luaext_key_loaded);
	lua_setfield(L, -2, "loaded");

	luaext_require_push_table(L, &luaext_key_preload);
	lua_setfield(L, -2, "preload");

	/*
	 * A joined, read-only rendering of modulePaths. Informational: nothing reads
	 * it back, and assigning to it changes no search. Upstream's package.path IS
	 * the search list, and making ours writable would let a script point the
	 * search wherever it liked -- so it reports rather than configures.
	 */
	{
		smart_str joined = {0};
		zval *entry;
		bool first = true;

		if (Z_TYPE(sandbox->module_paths_zv) == IS_ARRAY) {
			ZEND_HASH_FOREACH_VAL(Z_ARRVAL(sandbox->module_paths_zv), entry)
			{
				if (Z_TYPE_P(entry) != IS_STRING) {
					continue;
				}

				if (!first) {
					smart_str_appendc(&joined, ';');
				}

				smart_str_appendl(&joined, Z_STRVAL_P(entry), Z_STRLEN_P(entry));
				first = false;
			}
			ZEND_HASH_FOREACH_END();
		}

		smart_str_0(&joined);

		if (joined.s != NULL) {
			lua_pushlstring(L, ZSTR_VAL(joined.s), ZSTR_LEN(joined.s));
		} else {
			lua_pushliteral(L, "");
		}

		smart_str_free(&joined);
	}

	lua_setfield(L, -2, "path");

	/*
	 * No cpath, no searchers, no loadlib. Every one of those exists to reach a
	 * shared object; see the header. The table is frozen so a script cannot add
	 * one back and cannot replace loaded/preload wholesale.
	 */
	lua_createtable(L, 0, 2);
	lua_pushboolean(L, 0);
	lua_setfield(L, -2, "__metatable");
	lua_setmetatable(L, -2);

	lua_setglobal(L, "package");

	return true;
}
