/*
 * luaext — fuzzing the virtual-path canonicaliser.
 *
 * This target exists because luaext_vfs_path.c is where a sandbox escape would
 * live. It parses attacker-controlled bytes, and everything downstream trusts
 * its answer to be inside the namespace.
 *
 * It is fuzzable at all because the module is pure: no PHP, no Lua, no request.
 * That is the whole reason for the purity rule in luaext_vfs_path.h -- a
 * normaliser tangled up with an interpreter can only be tested through one.
 *
 * The invariants asserted below are the ones a caller relies on. A crash is a
 * finding; so is any success that does not satisfy them, which is why they are
 * checked rather than left to a sanitizer to notice indirectly.
 */

#include "luaext_vfs_path.h"

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	/* Two quota bytes are taken from the input so the fuzzer can explore the
	 * bounded paths as well as the unbounded ones. */
	uint32_t max_length;
	uint32_t max_depth;
	char *out;
	size_t out_size;
	size_t out_len = 0;
	luaext_vfs_path_status status;

	if (size < 2) {
		return 0;
	}

	max_length = data[0];
	max_depth = data[1] & 0x1f;
	data += 2;
	size -= 2;

	/* The documented requirement on the caller: input + 2. Allocated exactly
	 * that, so a write past the promise is a heap overflow ASan will catch
	 * rather than something absorbed by slack. */
	out_size = size + 2;
	out = (char *)malloc(out_size);

	if (out == NULL) {
		return 0;
	}

	status = luaext_vfs_path_canonical((const char *)data, size, out, out_size, &out_len,
									   max_length, max_depth);

	if (status == LUAEXT_VFS_PATH_OK) {
		size_t index;
		size_t depth = 0;

		/* Rooted. Everything downstream joins onto this assumption. */
		assert(out[0] == '/');
		assert(out_len == strlen(out));
		assert(out_len < out_size);

		/* No trailing separator, and no empty component: "//" cannot appear. */
		assert(out_len == 1 || out[out_len - 1] != '/');

		for (index = 0; index < out_len; index++) {
			assert(out[index] != '\0');

			if (out[index] == '/') {
				depth++;
				assert(index + 1 == out_len || out[index + 1] != '/');
			}
		}

		/*
		 * THE invariant. No surviving "." or ".." component, because a backend
		 * joining these names onto anything would resolve them itself -- which
		 * is exactly the escape this module exists to prevent.
		 */
		assert(strstr(out, "/../") == NULL);
		assert(strstr(out, "/./") == NULL);
		assert(out_len < 3 || strcmp(out + out_len - 3, "/..") != 0);
		assert(out_len < 2 || strcmp(out + out_len - 2, "/.") != 0);

		/* The quotas were honoured, not merely consulted. */
		assert(max_length == 0 || out_len <= max_length);
		assert(max_depth == 0 || depth <= max_depth);
	}

	free(out);

	return 0;
}
