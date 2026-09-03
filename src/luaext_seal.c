/*
 * luaext — HMAC-SHA256 over bytecode blobs. See luaext_seal.h for the format
 * and for what sealing does and does not buy.
 *
 * SHA-256 is taken from ext/hash rather than vendored, and it is looked up
 * through php_hash_fetch_ops() rather than by naming php_hash_sha256_ops.
 *
 * The header declares that struct extern, but the PHP binary does NOT export
 * it -- it is a local symbol, so a module referencing it fails to dlopen with
 * "symbol not found in flat namespace". php_hash_fetch_ops IS exported, the
 * same way php_random_bytes is, and it is the only supported way in.
 *
 * Fetched once at MINIT: the algorithm table is built during ext/hash's own
 * startup and is read-only afterwards, so one lookup serves every request and
 * every thread.
 */

#include "luaext_seal.h"

#include <string.h>

#include <ext/hash/php_hash.h>

/* Resolved at MINIT; never changes afterwards. */
static const php_hash_ops *luaext_seal_sha256;

void luaext_seal_startup(void)
{
	zend_string *algo = zend_string_init("sha256", sizeof("sha256") - 1, 1);

	luaext_seal_sha256 = php_hash_fetch_ops(algo);
	zend_string_release(algo);

	/*
	 * ext/hash is always built and always registers sha256, so this is a
	 * "cannot happen". Said out loud anyway: silently leaving the pointer NULL
	 * would turn every seal into a null dereference far from the cause.
	 */
	if (luaext_seal_sha256 == NULL) {
		zend_error(E_CORE_ERROR, "luaext: ext/hash does not provide sha256, so bytecode "
								 "cannot be sealed or verified");
	}
}

bool luaext_seal_is_sealed(const char *blob, size_t length)
{
	return blob != NULL && length >= LUAEXT_SEAL_HEADER_LEN &&
		   memcmp(blob, LUAEXT_SEAL_MAGIC, LUAEXT_SEAL_MAGIC_LEN) == 0;
}

/*
 * HMAC-SHA256, the textbook construction: H((K ^ opad) || H((K ^ ipad) || m)).
 *
 * A key longer than the block is hashed down first; a shorter one is zero
 * padded. Both are part of the standard, not shortcuts.
 */
static void luaext_seal_hmac(const char *key, size_t key_len, unsigned char version,
							 const char *payload, size_t payload_len,
							 unsigned char digest[LUAEXT_SEAL_MAC_LEN])
{
	const php_hash_ops *ops = luaext_seal_sha256;
	unsigned char block[64];
	unsigned char inner[LUAEXT_SEAL_MAC_LEN];
	unsigned char pad[64];
	void *context;
	size_t index;

	LUAEXT_ASSERT(ops->digest_size == LUAEXT_SEAL_MAC_LEN);
	LUAEXT_ASSERT(ops->block_size == sizeof(block));

	context = emalloc(ops->context_size);
	memset(block, 0, sizeof(block));

	if (key_len > sizeof(block)) {
		ops->hash_init(context, NULL);
		ops->hash_update(context, (const unsigned char *)key, key_len);
		ops->hash_final(block, context);
	} else {
		memcpy(block, key, key_len);
	}

	for (index = 0; index < sizeof(block); index++) {
		pad[index] = (unsigned char)(block[index] ^ 0x36);
	}

	ops->hash_init(context, NULL);
	ops->hash_update(context, pad, sizeof(pad));
	ops->hash_update(context, &version, 1);

	if (payload_len > 0) {
		ops->hash_update(context, (const unsigned char *)payload, payload_len);
	}

	ops->hash_final(inner, context);

	for (index = 0; index < sizeof(block); index++) {
		pad[index] = (unsigned char)(block[index] ^ 0x5c);
	}

	ops->hash_init(context, NULL);
	ops->hash_update(context, pad, sizeof(pad));
	ops->hash_update(context, inner, sizeof(inner));
	ops->hash_final(digest, context);

	/* The derived block and both pads are key material. */
	ZEND_SECURE_ZERO(block, sizeof(block));
	ZEND_SECURE_ZERO(pad, sizeof(pad));
	ZEND_SECURE_ZERO(context, ops->context_size);
	efree(context);
}

/*
 * Compare in constant time.
 *
 * This looks like a clumsy memcmp and is deliberately not one: memcmp returns
 * as soon as it finds a difference, so how long it took reveals how many
 * leading bytes matched. That is enough to recover a MAC a byte at a time
 * across many attempts. Accumulating every difference makes the running time
 * depend only on the length, which here is a fixed 32.
 */
static bool luaext_seal_equal(const unsigned char *left, const unsigned char *right, size_t length)
{
	unsigned char difference = 0;
	size_t index;

	for (index = 0; index < length; index++) {
		difference |= (unsigned char)(left[index] ^ right[index]);
	}

	return difference == 0;
}

zend_string *luaext_seal_wrap(const char *payload, size_t payload_len, const char *key,
							  size_t key_len)
{
	zend_string *sealed = zend_string_alloc(LUAEXT_SEAL_HEADER_LEN + payload_len, 0);
	char *out = ZSTR_VAL(sealed);
	unsigned char digest[LUAEXT_SEAL_MAC_LEN];

	memcpy(out, LUAEXT_SEAL_MAGIC, LUAEXT_SEAL_MAGIC_LEN);
	out[LUAEXT_SEAL_MAGIC_LEN] = (char)LUAEXT_SEAL_VERSION;

	luaext_seal_hmac(key, key_len, (unsigned char)LUAEXT_SEAL_VERSION, payload, payload_len,
					 digest);

	memcpy(out + LUAEXT_SEAL_MAGIC_LEN + 1, digest, sizeof(digest));

	if (payload_len > 0) {
		memcpy(out + LUAEXT_SEAL_HEADER_LEN, payload, payload_len);
	}

	ZSTR_VAL(sealed)[ZSTR_LEN(sealed)] = '\0';

	return sealed;
}

bool luaext_seal_open(const char *blob, size_t length, const char *key, size_t key_len,
					  const char **payload, size_t *payload_len)
{
	unsigned char expected[LUAEXT_SEAL_MAC_LEN];
	unsigned char version;
	const char *body;
	size_t body_len;

	if (!luaext_seal_is_sealed(blob, length)) {
		return false;
	}

	version = (unsigned char)blob[LUAEXT_SEAL_MAGIC_LEN];

	if (version != LUAEXT_SEAL_VERSION) {
		return false;
	}

	body = blob + LUAEXT_SEAL_HEADER_LEN;
	body_len = length - LUAEXT_SEAL_HEADER_LEN;

	luaext_seal_hmac(key, key_len, version, body, body_len, expected);

	if (!luaext_seal_equal(expected, (const unsigned char *)blob + LUAEXT_SEAL_MAGIC_LEN + 1,
						   LUAEXT_SEAL_MAC_LEN)) {
		return false;
	}

	*payload = body;
	*payload_len = body_len;

	return true;
}
