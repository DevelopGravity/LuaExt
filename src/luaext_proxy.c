/*
 * luaext — PHP object proxies: the class registry.
 *
 * Registration is where every rule is enforced, so nothing downstream has to
 * re-check: by the time a record exists, its method tables hold only public,
 * non-abstract zend_functions, its table names cannot collide, and its class
 * is a concrete, nameable one. A refusal here leaves the script's view of the
 * world untouched — selection happens entirely before anything is stored.
 *
 * Storage is persistent (pemalloc, persistent strings): the registry is torn
 * down on the close path, which can run from the RSHUTDOWN sweep after the
 * request allocator's arena is on its way out.
 */

#include "luaext_proxy.h"

#include "luaext_phpcall.h"

#include <Zend/zend_attributes.h>
#include <Zend/zend_exceptions.h>

/* -------------------------------------------------------------------------
 * Small helpers
 * ---------------------------------------------------------------------- */

/* A persistent copy of a (possibly request-allocated) string. */
static zend_string *luaext_proxy_pstr(const zend_string *source)
{
	return zend_string_init(ZSTR_VAL(source), ZSTR_LEN(source), 1);
}

/*
 * Why a method cannot be exposed, or NULL. Deliberately NOT the phpcall
 * version: statics are first-class citizens here (they land on the class
 * table), while registerObject() has no receiver-free surface to put one on.
 */
static const char *luaext_proxy_method_refusal(const zend_function *method)
{
	if (!(method->common.fn_flags & ZEND_ACC_PUBLIC)) {
		return "it is not public";
	}

	if (method->common.fn_flags & ZEND_ACC_ABSTRACT) {
		return "it is abstract";
	}

	return NULL;
}

/*
 * Add one selected method under its Lua name, refusing a duplicate key. The
 * instance and static tables are separate namespaces (metatable __index vs
 * the class table), so only same-table duplicates are collisions.
 */
static bool luaext_proxy_table_add(HashTable *table, const zend_class_entry *ce,
								   zend_string *lua_name, zend_function *method)
{
	zend_string *key = luaext_proxy_pstr(lua_name);

	if (zend_hash_add_ptr(table, key, method) == NULL) {
		zend_string_release(key);
		zend_throw_exception_ex(luaext_ce_configuration_error, 0,
								"Two exposures of %s both want the Lua name \"%s\"",
								ZSTR_VAL(ce->name), ZSTR_VAL(lua_name));
		return false;
	}

	/* The table holds its own reference now. */
	zend_string_release(key);

	return true;
}

/* -------------------------------------------------------------------------
 * Selection
 *
 * Two routes, mirroring registerObject(): an explicit allowlist overrides
 * every attribute. Constructors and __toString are routed to their dedicated
 * fields rather than the method tables, whichever route selected them.
 * ---------------------------------------------------------------------- */

/* Record an exposed constructor under `name`, refusing a class-table clash. */
static bool luaext_proxy_set_constructor(luaext_proxy_class *record, const zend_class_entry *ce,
										 zend_function *constructor, zend_string *name)
{
	record->constructor = constructor;
	record->constructor_lua_name = luaext_proxy_pstr(name);

	if (zend_hash_exists(record->static_methods, record->constructor_lua_name)) {
		zend_throw_exception_ex(luaext_ce_configuration_error, 0,
								"Two exposures of %s both want the Lua name \"%s\"",
								ZSTR_VAL(ce->name), ZSTR_VAL(name));
		return false;
	}

	return true;
}

/* Every method carrying #[LuaMethod], honouring the name it asks for. */
static bool luaext_proxy_collect_attributed(luaext_proxy_class *record, zend_class_entry *ce)
{
	zend_string *marker = zend_string_tolower(luaext_ce_lua_method_attribute->name);
	zend_function *method;
	zend_function *constructor = NULL;
	zend_string *constructor_name = NULL;
	bool collected = true;

	ZEND_HASH_MAP_FOREACH_PTR(&ce->function_table, method)
	{
		zend_attribute *attribute = zend_get_attribute(method->common.attributes, marker);
		zend_string *lua_name;
		const char *refusal;
		bool overridden;

		if (attribute == NULL) {
			continue;
		}

		refusal = luaext_proxy_method_refusal(method);

		if (refusal != NULL) {
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

		/* An attribute with no name override resolves to the method's own. */
		overridden = !zend_string_equals(lua_name, method->common.function_name);

		if (method == ce->constructor) {
			constructor = method;

			if (overridden) {
				constructor_name = lua_name;
			} else {
				zend_string_release(lua_name);
				constructor_name = ZSTR_INIT_LITERAL("new", 0);
			}

			continue;
		}

		if (ce->__tostring != NULL && method == ce->__tostring) {
			/* Marking __toString activates the metamethod; a name override
			 * additionally exposes it as an ordinary method. */
			record->to_string = method;

			if (overridden) {
				collected = luaext_proxy_table_add(record->instance_methods, ce, lua_name, method);
			}

			zend_string_release(lua_name);

			if (!collected) {
				break;
			}

			continue;
		}

		if (method->common.fn_flags & ZEND_ACC_STATIC) {
			collected = luaext_proxy_table_add(record->static_methods, ce, lua_name, method);
		} else {
			collected = luaext_proxy_table_add(record->instance_methods, ce, lua_name, method);
		}

		zend_string_release(lua_name);

		if (!collected) {
			break;
		}
	}
	ZEND_HASH_FOREACH_END();

	zend_string_release(marker);

	if (collected && constructor != NULL) {
		collected = luaext_proxy_set_constructor(record, ce, constructor, constructor_name);
	}

	if (constructor_name != NULL) {
		zend_string_release(constructor_name);
	}

	return collected;
}

/* The caller's explicit allowlist, which overrides every attribute. */
static bool luaext_proxy_collect_allowlist(luaext_proxy_class *record, zend_class_entry *ce,
										   HashTable *allowlist)
{
	zval *entry;
	zend_function *constructor = NULL;

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

		/* Looked up in the class's own table, not resolved as a callable: a
		 * name the class does not declare must be an error, never a detour
		 * through __call. _lc because function_table keys are lowercase. */
		method = (zend_function *)zend_hash_str_find_ptr_lc(
			&ce->function_table, ZSTR_VAL(requested), ZSTR_LEN(requested));

		if (method == NULL) {
			zend_throw_exception_ex(luaext_ce_configuration_error, 0,
									"%s has no method %s() to expose to Lua", ZSTR_VAL(ce->name),
									ZSTR_VAL(requested));
			return false;
		}

		if (method == ce->constructor) {
			constructor = method;
			continue;
		}

		if (ce->__tostring != NULL && method == ce->__tostring) {
			record->to_string = method;
			continue;
		}

		refusal = luaext_proxy_method_refusal(method);

		if (refusal != NULL) {
			zend_throw_exception_ex(luaext_ce_configuration_error, 0,
									"%s::%s() cannot be exposed to Lua: %s", ZSTR_VAL(ce->name),
									ZSTR_VAL(requested), refusal);
			return false;
		}

		if (method->common.fn_flags & ZEND_ACC_STATIC) {
			if (!luaext_proxy_table_add(record->static_methods, ce, requested, method)) {
				return false;
			}
		} else {
			if (!luaext_proxy_table_add(record->instance_methods, ce, requested, method)) {
				return false;
			}
		}
	}
	ZEND_HASH_FOREACH_END();

	if (constructor != NULL) {
		zend_string *name = ZSTR_INIT_LITERAL("new", 0);
		bool added = luaext_proxy_set_constructor(record, ce, constructor, name);

		zend_string_release(name);

		if (!added) {
			return false;
		}
	}

	return true;
}

/* -------------------------------------------------------------------------
 * The registry
 * ---------------------------------------------------------------------- */

static void luaext_proxy_class_free(luaext_proxy_class *record)
{
	if (record->instance_methods != NULL) {
		zend_hash_destroy(record->instance_methods);
		pefree(record->instance_methods, 1);
	}

	if (record->static_methods != NULL) {
		zend_hash_destroy(record->static_methods);
		pefree(record->static_methods, 1);
	}

	if (record->lua_name != NULL) {
		zend_string_release(record->lua_name);
	}

	if (record->constructor_lua_name != NULL) {
		zend_string_release(record->constructor_lua_name);
	}

	pefree(record, 1);
}

bool luaext_proxy_register(luaext_sandbox *sandbox, zend_string *class_name, HashTable *allowlist,
						   zend_string *lua_name, HashTable *operators)
{
	zend_class_entry *ce;
	luaext_proxy_class *walk;
	luaext_proxy_class *record;
	zend_string *resolved_name;
	bool collected;

	ce = zend_lookup_class(class_name);

	if (ce == NULL) {
		/* An autoloader that threw already explains the failure better. */
		if (EG(exception) == NULL) {
			zend_throw_exception_ex(luaext_ce_configuration_error, 0,
									"Cannot register %s: the class does not exist",
									ZSTR_VAL(class_name));
		}

		return false;
	}

	if (ce->ce_flags & ZEND_ACC_INTERFACE) {
		zend_throw_exception_ex(
			luaext_ce_configuration_error, 0,
			"Cannot register interface %s: only classes have instances to proxy",
			ZSTR_VAL(ce->name));
		return false;
	}

	if (ce->ce_flags & ZEND_ACC_ENUM) {
		zend_throw_exception_ex(luaext_ce_configuration_error, 0,
								"Cannot register enum %s: enum cases are process-lifetime "
								"singletons, not instances a proxy can own",
								ZSTR_VAL(ce->name));
		return false;
	}

	if (ce->ce_flags & ZEND_ACC_ANON_CLASS) {
		zend_throw_exception(luaext_ce_configuration_error,
							 "Cannot register an anonymous class: it has no usable Lua name", 0);
		return false;
	}

	for (walk = sandbox->proxy_classes; walk != NULL; walk = walk->next) {
		if (walk->ce == ce) {
			zend_throw_exception_ex(luaext_ce_configuration_error, 0,
									"%s is already registered on this sandbox", ZSTR_VAL(ce->name));
			return false;
		}
	}

	/*
	 * The Lua name: parameter, else the unqualified class name. Recorded even
	 * for a class that plants no table, so name collisions are refused by one
	 * rule regardless of what a later registration exposes.
	 */
	if (lua_name != NULL) {
		resolved_name = luaext_proxy_pstr(lua_name);
	} else {
		const char *backslash =
			(const char *)zend_memrchr(ZSTR_VAL(ce->name), '\\', ZSTR_LEN(ce->name));
		const char *short_name = backslash != NULL ? backslash + 1 : ZSTR_VAL(ce->name);

		resolved_name = zend_string_init(
			short_name, ZSTR_LEN(ce->name) - (size_t)(short_name - ZSTR_VAL(ce->name)), 1);
	}

	for (walk = sandbox->proxy_classes; walk != NULL; walk = walk->next) {
		if (zend_string_equals(walk->lua_name, resolved_name)) {
			zend_string_release(resolved_name);
			zend_throw_exception_ex(
				luaext_ce_configuration_error, 0,
				"The Lua name \"%s\" is already taken by a previously registered class",
				lua_name != NULL ? ZSTR_VAL(lua_name) : ZSTR_VAL(ce->name));
			return false;
		}
	}

	record = pecalloc(1, sizeof(*record), 1);
	record->ce = ce;
	record->lua_name = resolved_name;
	record->instance_methods = pemalloc(sizeof(HashTable), 1);
	zend_hash_init(record->instance_methods, 8, NULL, NULL, 1);
	record->static_methods = pemalloc(sizeof(HashTable), 1);
	zend_hash_init(record->static_methods, 8, NULL, NULL, 1);

	if (allowlist != NULL) {
		collected = luaext_proxy_collect_allowlist(record, ce, allowlist);
	} else {
		collected = luaext_proxy_collect_attributed(record, ce);
	}

	/*
	 * Instances of an abstract class cannot exist, so an exposed constructor
	 * on one is a mistake stored — and the class may still register for
	 * proxying, since concrete subclasses wrap as their nearest registered
	 * ancestor.
	 */
	if (collected && record->constructor != NULL &&
		(ce->ce_flags & ZEND_ACC_EXPLICIT_ABSTRACT_CLASS)) {
		zend_throw_exception_ex(luaext_ce_configuration_error, 0,
								"Cannot expose the constructor of abstract %s", ZSTR_VAL(ce->name));
		collected = false;
	}

	/*
	 * Shape check only: keys are method names, values are Operator cases.
	 * Full slot validation (arity, bool returns, duplicates) is the operator
	 * subsystem's, which also fills op_methods.
	 */
	if (collected && operators != NULL) {
		zend_string *op_key;
		zval *op_value;

		ZEND_HASH_FOREACH_STR_KEY_VAL(operators, op_key, op_value)
		{
			ZVAL_DEREF(op_value);

			if (op_key == NULL || ZSTR_LEN(op_key) == 0 || Z_TYPE_P(op_value) != IS_OBJECT ||
				!instanceof_function(Z_OBJCE_P(op_value), luaext_ce_operator)) {
				zend_throw_exception_ex(
					luaext_ce_configuration_error, 0,
					"The operator map for %s must map method names to Operator cases",
					ZSTR_VAL(ce->name));
				collected = false;
				break;
			}
		}
		ZEND_HASH_FOREACH_END();
	}

	if (collected && zend_hash_num_elements(record->instance_methods) == 0 &&
		zend_hash_num_elements(record->static_methods) == 0 && record->constructor == NULL &&
		record->to_string == NULL &&
		(operators == NULL || zend_hash_num_elements(operators) == 0)) {
		zend_throw_exception_ex(luaext_ce_configuration_error, 0,
								"Nothing of %s is exposed: no method carries #[LuaMethod], no "
								"allowlist was given, and no operator is mapped",
								ZSTR_VAL(ce->name));
		collected = false;
	}

	if (!collected) {
		luaext_proxy_class_free(record);
		return false;
	}

	record->next = sandbox->proxy_classes;
	sandbox->proxy_classes = record;

	return true;
}

luaext_proxy_class *luaext_proxy_find(const luaext_sandbox *sandbox, const zend_class_entry *ce)
{
	const zend_class_entry *ancestor;

	/*
	 * Nearest registered ancestor: the instance's own class first, then each
	 * parent in turn. Interfaces are never consulted — "nearest" has no
	 * meaning across a lattice.
	 */
	for (ancestor = ce; ancestor != NULL; ancestor = ancestor->parent) {
		luaext_proxy_class *record;

		for (record = sandbox->proxy_classes; record != NULL; record = record->next) {
			if (record->ce == ancestor) {
				return record;
			}
		}
	}

	return NULL;
}

void luaext_proxy_shutdown(luaext_sandbox *sandbox)
{
	luaext_proxy_class *record = sandbox->proxy_classes;

	sandbox->proxy_classes = NULL;

	while (record != NULL) {
		luaext_proxy_class *next = record->next;

		luaext_proxy_class_free(record);
		record = next;
	}
}
