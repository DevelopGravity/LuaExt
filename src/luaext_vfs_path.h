/*
 * luaext — canonicalising a virtual path.
 *
 * PURITY RULE, and it is the same discipline the watchdog follows for the same
 * reason: this file includes neither php.h nor lua.h, and must not gain either.
 * A path normaliser is where sandbox escapes live, so it is worth being able to
 * reason about, test and fuzz it as a pure string function with no interpreter
 * and no request behind it.
 *
 * THE NAMESPACE IS VIRTUAL. There is no host filesystem here and no syscall at
 * the end of it: a canonical path is a name the backend is asked about, and the
 * backend is PHP code the host wrote. That is what makes lexical resolution of
 * `..` correct rather than dangerous -- there are no symlinks to race, because
 * there is nothing to symlink.
 *
 * The rules, in the order they matter:
 *
 *   - Rooted at '/'. A relative path is resolved against '/', not against
 *     anything a script could move.
 *   - '.' is dropped; '..' pops the previous component.
 *   - A '..' that would pop past the root is an ERROR, never a silent clamp to
 *     '/'. Clamping is how "../../../etc/passwd" becomes "/etc/passwd" and looks
 *     like it was always meant to be there.
 *   - '\\' is an ordinary character in a name. It is not a separator, and it is
 *     not translated, because the namespace is not Windows' and a backend keyed
 *     on these names must see exactly what the script wrote.
 *   - A NUL byte is rejected outright: Lua strings carry them happily and C
 *     APIs beneath a careless backend do not.
 *   - Length and depth are bounded before a backend is ever consulted.
 */

#ifndef LUAEXT_VFS_PATH_H
#define LUAEXT_VFS_PATH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * The longest input this module will look at, independent of any quota.
 *
 * A ceiling exists so a caller can size its output buffer from the input length
 * without a script being able to name a length that makes that arithmetic
 * interesting. VfsQuota::$maxPathLength is the tighter, configurable bound; this
 * is the one that always applies.
 */
#define LUAEXT_VFS_PATH_MAX_INPUT 8192u

/* Why a canonicalisation failed. The caller turns this into a message. */
typedef enum {
	LUAEXT_VFS_PATH_OK = 0,
	LUAEXT_VFS_PATH_EMPTY,	   /* nothing, or only separators and dots */
	LUAEXT_VFS_PATH_NUL,	   /* embedded NUL byte */
	LUAEXT_VFS_PATH_ESCAPE,	   /* '..' above the root */
	LUAEXT_VFS_PATH_TOO_LONG,  /* over the configured length */
	LUAEXT_VFS_PATH_TOO_DEEP,  /* over the configured depth */
	LUAEXT_VFS_PATH_COMPONENT, /* a component is unusable on its own */
} luaext_vfs_path_status;

/*
 * Canonicalise `input` into `out`.
 *
 * `out_size` must be at least `input_len + 2`: the result is never longer than
 * the input plus a leading '/' and a terminator, since canonicalisation only
 * ever removes components.
 *
 * `max_length` and `max_depth` of 0 mean "no bound from the quota"; the caller
 * still gets the buffer bound above. On success `out` holds a path that always
 * begins with '/', never ends with '/' unless it IS "/", and contains no empty,
 * "." or ".." component.
 */
luaext_vfs_path_status luaext_vfs_path_canonical(const char *input, size_t input_len, char *out,
												 size_t out_size, size_t *out_len,
												 uint32_t max_length, uint32_t max_depth);

/* A short, stable phrase naming the failure, for an error message. */
const char *luaext_vfs_path_reason(luaext_vfs_path_status status);

#endif /* LUAEXT_VFS_PATH_H */
