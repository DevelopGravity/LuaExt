/*
 * luaext — canonicalising a virtual path. See luaext_vfs_path.h for the rules
 * and for why this file is pure.
 */

#include "luaext_vfs_path.h"

#include <string.h>

/*
 * Component offsets are recorded rather than the components copied, so the walk
 * needs no allocation and no second buffer. A '..' pops one entry; the result is
 * assembled once at the end from whatever survived.
 *
 * Bounded because the array lives on the C stack. A path with more components
 * than this is refused as too deep even when the quota allows it -- an absolute
 * ceiling on recursion is not a limit a host should be able to configure away.
 */
#define LUAEXT_VFS_PATH_MAX_COMPONENTS 256u

typedef struct {
	const char *at;
	size_t len;
} luaext_vfs_path_part;

const char *luaext_vfs_path_reason(luaext_vfs_path_status status)
{
	switch (status) {
	case LUAEXT_VFS_PATH_OK:
		return "is usable";

	case LUAEXT_VFS_PATH_EMPTY:
		return "names nothing once '.' and separators are resolved";

	case LUAEXT_VFS_PATH_NUL:
		return "contains a NUL byte";

	case LUAEXT_VFS_PATH_ESCAPE:
		return "climbs above the root with '..'";

	case LUAEXT_VFS_PATH_TOO_LONG:
		return "is longer than the filesystem quota allows";

	case LUAEXT_VFS_PATH_TOO_DEEP:
		return "nests deeper than the filesystem quota allows";

	case LUAEXT_VFS_PATH_COMPONENT:
		return "has a component that cannot be used as a name";
	}

	return "cannot be used";
}

luaext_vfs_path_status luaext_vfs_path_canonical(const char *input, size_t input_len, char *out,
												 size_t out_size, size_t *out_len,
												 uint32_t max_length, uint32_t max_depth)
{
	luaext_vfs_path_part parts[LUAEXT_VFS_PATH_MAX_COMPONENTS];
	size_t count = 0;
	size_t cursor = 0;
	size_t written;
	size_t index;

	if (out == NULL || out_size < 2) {
		return LUAEXT_VFS_PATH_TOO_LONG;
	}

	/*
	 * Length first, before any walking. The quota exists so a backend is never
	 * handed something enormous, and checking it after the work would only
	 * protect the backend from what this function already survived.
	 */
	if (max_length != 0 && input_len > (size_t)max_length) {
		return LUAEXT_VFS_PATH_TOO_LONG;
	}

	if (input_len + 2 > out_size) {
		return LUAEXT_VFS_PATH_TOO_LONG;
	}

	/*
	 * A NUL anywhere, not just a leading one. Lua strings are byte strings and
	 * carry NULs happily; a backend that passes the name to anything C-shaped
	 * would see it truncated, so "/safe\0/../../etc" must never reach one.
	 */
	if (memchr(input, '\0', input_len) != NULL) {
		return LUAEXT_VFS_PATH_NUL;
	}

	while (cursor < input_len) {
		size_t start;
		size_t length;

		/* Runs of separators collapse: "//a///b" is "/a/b". */
		while (cursor < input_len && input[cursor] == '/') {
			cursor++;
		}

		if (cursor >= input_len) {
			break;
		}

		start = cursor;

		while (cursor < input_len && input[cursor] != '/') {
			cursor++;
		}

		length = cursor - start;

		if (length == 1 && input[start] == '.') {
			continue;
		}

		if (length == 2 && input[start] == '.' && input[start + 1] == '.') {
			if (count == 0) {
				/*
				 * The rule this whole file exists for. Clamping to the root here
				 * is what turns "../../../etc/passwd" into "/etc/passwd" and
				 * makes it look like a path the script was entitled to name.
				 * Refusing keeps the script's mistake the script's mistake.
				 */
				return LUAEXT_VFS_PATH_ESCAPE;
			}

			count--;
			continue;
		}

		/*
		 * A component made only of dots beyond "." and ".." -- "...", "...." --
		 * is refused rather than passed through. It means nothing in this
		 * namespace, and several real filesystems treat it specially, so a
		 * backend built on one would disagree with this layer about what the
		 * name refers to.
		 */
		{
			size_t dots = 0;

			while (dots < length && input[start + dots] == '.') {
				dots++;
			}

			if (dots == length) {
				return LUAEXT_VFS_PATH_COMPONENT;
			}
		}

		/*
		 * The absolute ceiling, checked here because it bounds THIS function's
		 * stack array rather than the answer. The configured quota is checked
		 * against the finished path instead: see below.
		 */
		if (count >= LUAEXT_VFS_PATH_MAX_COMPONENTS) {
			return LUAEXT_VFS_PATH_TOO_DEEP;
		}

		parts[count].at = input + start;
		parts[count].len = length;
		count++;
	}

	/*
	 * Depth is judged on what SURVIVED, not on how far the input wandered.
	 * "/a/b/c/../../d" is two components deep when it reaches a backend, and a
	 * quota describing what may exist in the namespace should say yes to it --
	 * the transient three are this function's business, and the fixed ceiling
	 * above is what bounds them.
	 */
	if (max_depth != 0 && count > (size_t)max_depth) {
		return LUAEXT_VFS_PATH_TOO_DEEP;
	}

	if (count == 0) {
		/*
		 * "/" itself, or "." or "///". A directory-ish name is not a file name,
		 * and every caller here wants a file, so this is the caller's problem to
		 * report rather than a path to hand onward.
		 */
		return LUAEXT_VFS_PATH_EMPTY;
	}

	written = 0;

	for (index = 0; index < count; index++) {
		out[written++] = '/';
		memcpy(out + written, parts[index].at, parts[index].len);
		written += parts[index].len;
	}

	out[written] = '\0';

	if (max_length != 0 && written > (size_t)max_length) {
		return LUAEXT_VFS_PATH_TOO_LONG;
	}

	if (out_len != NULL) {
		*out_len = written;
	}

	return LUAEXT_VFS_PATH_OK;
}
