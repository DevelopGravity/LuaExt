/*
 * luaext — configuration value objects and the policy they resolve to.
 *
 * Capabilities, Limits, VfsQuota and SandboxConfig are immutable PHP objects
 * the host builds; luaext_policy is the flat C form the interpreter consults.
 * Resolution happens once, at construction, so nothing that shapes a
 * lua_State can change under a running script.
 */

#ifndef LUAEXT_CONFIG_H
#define LUAEXT_CONFIG_H

#include "luaext_types.h"

/* Install object handlers for the configuration value objects. From MINIT. */
void luaext_config_startup(void);

/*
 * Fill `policy` from a SandboxConfig, or from the untrusted defaults when
 * `config` is NULL.
 *
 * Returns false with a thrown exception when the configuration cannot be
 * satisfied. The refusals are deliberate and belong here rather than at the
 * point of use:
 *
 *   - debugHooks together with a CPU limit, because a script that can install
 *     its own debug hook can displace the one the limit depends on;
 *   - a fixed seed without deterministic mode, because pinning the string
 *     hash seed forfeits hash-flooding protection and must be asked for;
 *   - the vfs capability with no FileSystem to back it.
 */
bool luaext_config_resolve(zval *config, luaext_policy *policy);

/*
 * Build a Capabilities object with the given flags, for untrusted()/trusted()
 * and for with(). Returns the new object in `out`.
 */
void luaext_config_capabilities_create(uint32_t caps, zval *out);

/*
 * Apply a with(...$overrides) argument list to an existing value object.
 *
 * `named` is the caller's named arguments; each key must name a declared
 * property of `ce` or a ConfigurationError is thrown naming the offender.
 * Positional arguments are refused: with() exists to change one field by
 * name, and a positional form would silently depend on declaration order.
 */
bool luaext_config_with(zend_class_entry *ce, zend_object *source, HashTable *named, zval *out);

/* Snapshot a sandbox's counters as a SandboxStats object. */
void luaext_config_stats_create(const luaext_sandbox *sandbox, zval *out);

#endif /* LUAEXT_CONFIG_H */
