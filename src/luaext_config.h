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
 *   - the vfs capability with no FileSystem to back it;
 *   - a negative limit, because (size_t)-1 is the widest possible budget and a
 *     typo must not quietly become one.
 *
 * Resolution is pure, so it is cheap to run more than once. SandboxConfig's own
 * constructor runs it over its arguments before committing them, which is what
 * makes a refusal land on the line that built the bad configuration; the
 * sandbox runs it again to obtain the policy it will actually enforce.
 */
bool luaext_config_resolve(zval *config, luaext_policy *policy);

/*
 * Build a Capabilities object with the given flags, for untrusted()/trusted().
 * Returns the new object in `out`.
 *
 * Not used by with(), which copies the source object's fields directly: a
 * bitset cannot carry osEnvAllowList, so a round trip through one would drop it.
 */
void luaext_config_capabilities_create(uint32_t caps, zval *out);

/*
 * Apply a with(...$overrides) argument list to an existing value object.
 *
 * `named` is the caller's named arguments; each key must name a declared
 * property of `ce` or a ConfigurationError is thrown naming the offender, and
 * each value must satisfy that property's declared type. On failure `out` is
 * set to null and an exception is pending.
 *
 * Positional arguments are refused as well — with() exists to change one field
 * by name, and a positional form would silently depend on declaration order —
 * but that check lives in the with() methods themselves, which are the only
 * things that can see an argument's position.
 */
bool luaext_config_with(zend_class_entry *ce, zend_object *source, HashTable *named, zval *out);

/* Snapshot a sandbox's counters as a SandboxStats object. */
void luaext_config_stats_create(const luaext_sandbox *sandbox, zval *out);

/*
 * Read a Limits object into the enforceable form, throwing on a value the
 * conversion refuses.
 *
 * The same function SandboxConfig resolution uses, exposed so that
 * Sandbox::setLimits() applies exactly the rules the constructor applied. A
 * second implementation would be a second set of rules, and the divergence would
 * show up as a limit that means one thing at construction and another later.
 */
bool luaext_config_limits_read(zend_object *limits, luaext_limits *out);

/* Build a Limits object from the enforceable form. The inverse of the above. */
void luaext_config_limits_create(const luaext_limits *limits, zval *out);

/*
 * Build a ValidationResult. `message` and `chunk_name` are borrowed, and either
 * may be NULL. A `line` of 0 becomes null rather than zero, which is the honest
 * answer for a refusal that never reached the parser.
 */
void luaext_config_validation_create(zval *out, bool valid, zend_string *message, zend_long line,
									 const char *chunk_name);

#endif /* LUAEXT_CONFIG_H */
