/*
 * luaext — sealing bytecode. See luaext_seal.h for the format, the two modes,
 * and what each one does and does not buy.
 *
 * Both hashes come from ext/hash rather than being vendored, and they are
 * looked up through php_hash_fetch_ops() rather than by naming
 * php_hash_sha256_ops directly. The header declares those structs extern, but
 * the PHP binary does NOT export them -- they are local symbols, so a module
 * referencing one fails to dlopen with "symbol not found in flat namespace".
 * php_hash_fetch_ops IS exported, the same way php_random_bytes is.
 *
 * Fetched once at MINIT: the algorithm table is built during ext/hash's own
 * startup and is read-only afterwards, so one lookup serves every request and
 * every thread.
 */

#include "luaext_seal.h"

#include <string.h>

#include <ext/hash/php_hash.h>

/* Resolved at MINIT; never change afterwards. */
static const php_hash_ops *luaext_seal_sha256;
static const php_hash_ops *luaext_seal_xxh128;

static const php_hash_ops *luaext_seal_lookup(const char *name)
{
	zend_string *algo = zend_string_init(name, strlen(name), 1);
	const php_hash_ops *ops = php_hash_fetch_ops(algo);

	zend_string_release(algo);

	/*
	 * ext/hash is always built and registers both of these, so this is a
	 * "cannot happen". Said out loud anyway: silently leaving a NULL here would
	 * turn every seal into a null dereference far from the cause.
	 */
	if (ops == NULL) {
		zend_error(E_CORE_ERROR,
				   "luaext: ext/hash does not provide %s, so bytecode cannot be "
				   "sealed or verified",
				   name);
	}

	return ops;
}

void luaext_seal_startup(void)
{
	luaext_seal_sha256 = luaext_seal_lookup("sha256");
	luaext_seal_xxh128 = luaext_seal_lookup("xxh128");
}

/* Digest length for an algorithm, or 0 if it is not one we know. */
static size_t luaext_seal_digest_len(luaext_seal_algo algo)
{
	switch (algo) {
	case LUAEXT_SEAL_CHECKSUM:
		return luaext_seal_xxh128->digest_size;
	case LUAEXT_SEAL_AUTHENTICATED:
		return luaext_seal_sha256->digest_size;
	default:
		return 0;
	}
}

bool luaext_seal_is_sealed(const char *blob, size_t length)
{
	return blob != NULL && length >= LUAEXT_SEAL_PREFIX_LEN &&
		   memcmp(blob, LUAEXT_SEAL_MAGIC, LUAEXT_SEAL_MAGIC_LEN) == 0;
}

/* xxh128 over version || algo || payload. Unkeyed: this detects accidents, and
 * is not asked to detect anybody. */
static void luaext_seal_checksum(unsigned char version, unsigned char algo, const char *payload,
								 size_t payload_len, unsigned char *digest)
{
	const php_hash_ops *ops = luaext_seal_xxh128;
	void *context = emalloc(ops->context_size);

	ops->hash_init(context, NULL);
	ops->hash_update(context, &version, 1);
	ops->hash_update(context, &algo, 1);

	if (payload_len > 0) {
		ops->hash_update(context, (const unsigned char *)payload, payload_len);
	}

	ops->hash_final(digest, context);
	efree(context);
}

/*
 * HMAC-SHA256, the textbook construction: H((K ^ opad) || H((K ^ ipad) || m)).
 *
 * A key longer than the block is hashed down first; a shorter one is zero
 * padded. Both are part of the standard, not shortcuts. Verified byte-for-byte
 * against PHP's own hash_hmac('sha256', ...).
 */
static void luaext_seal_hmac(const char *key, size_t key_len, unsigned char version,
							 unsigned char algo, const char *payload, size_t payload_len,
							 unsigned char *digest)
{
	const php_hash_ops *ops = luaext_seal_sha256;
	unsigned char block[64];
	unsigned char inner[LUAEXT_SEAL_MAX_DIGEST];
	unsigned char pad[64];
	void *context;
	size_t index;

	LUAEXT_ASSERT(ops->block_size == sizeof(block));
	LUAEXT_ASSERT(ops->digest_size <= sizeof(inner));

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
	ops->hash_update(context, &algo, 1);

	if (payload_len > 0) {
		ops->hash_update(context, (const unsigned char *)payload, payload_len);
	}

	ops->hash_final(inner, context);

	for (index = 0; index < sizeof(block); index++) {
		pad[index] = (unsigned char)(block[index] ^ 0x5c);
	}

	ops->hash_init(context, NULL);
	ops->hash_update(context, pad, sizeof(pad));
	ops->hash_update(context, inner, ops->digest_size);
	ops->hash_final(digest, context);

	/* The derived block and both pads are key material. */
	ZEND_SECURE_ZERO(block, sizeof(block));
	ZEND_SECURE_ZERO(pad, sizeof(pad));
	ZEND_SECURE_ZERO(context, ops->context_size);
	efree(context);
}

static void luaext_seal_digest(luaext_seal_algo algo, const char *key, size_t key_len,
							   const char *payload, size_t payload_len, unsigned char *digest)
{
	if (algo == LUAEXT_SEAL_AUTHENTICATED) {
		luaext_seal_hmac(key, key_len, (unsigned char)LUAEXT_SEAL_VERSION, (unsigned char)algo,
						 payload, payload_len, digest);
	} else {
		luaext_seal_checksum((unsigned char)LUAEXT_SEAL_VERSION, (unsigned char)algo, payload,
							 payload_len, digest);
	}
}

/*
 * Compare in constant time.
 *
 * This looks like a clumsy memcmp and is deliberately not one: memcmp returns
 * as soon as it finds a difference, so how long it took reveals how many
 * leading bytes matched. That is enough to recover a digest a byte at a time
 * across many attempts. Accumulating every difference makes the running time
 * depend only on the length.
 *
 * Applied to the checksum too. It buys nothing there -- an attacker who wants a
 * matching xxh128 just computes one -- but a comparison that is constant time
 * only sometimes is a trap for whoever edits this next.
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

zend_string *luaext_seal_wrap(const char *payload, size_t payload_len, luaext_seal_algo algo,
							  const char *key, size_t key_len)
{
	size_t digest_len = luaext_seal_digest_len(algo);
	zend_string *sealed;
	char *out;

	LUAEXT_ASSERT(digest_len > 0);

	sealed = zend_string_alloc(LUAEXT_SEAL_PREFIX_LEN + digest_len + payload_len, 0);
	out = ZSTR_VAL(sealed);

	memcpy(out, LUAEXT_SEAL_MAGIC, LUAEXT_SEAL_MAGIC_LEN);
	out[LUAEXT_SEAL_MAGIC_LEN] = (char)LUAEXT_SEAL_VERSION;
	out[LUAEXT_SEAL_MAGIC_LEN + 1] = (char)algo;

	luaext_seal_digest(algo, key, key_len, payload, payload_len,
					   (unsigned char *)out + LUAEXT_SEAL_PREFIX_LEN);

	if (payload_len > 0) {
		memcpy(out + LUAEXT_SEAL_PREFIX_LEN + digest_len, payload, payload_len);
	}

	ZSTR_VAL(sealed)[ZSTR_LEN(sealed)] = '\0';

	return sealed;
}

bool luaext_seal_open(const char *blob, size_t length, luaext_seal_algo expected, const char *key,
					  size_t key_len, const char **payload, size_t *payload_len)
{
	unsigned char computed[LUAEXT_SEAL_MAX_DIGEST];
	size_t digest_len = luaext_seal_digest_len(expected);
	const char *body;
	size_t body_len;

	if (!luaext_seal_is_sealed(blob, length) || digest_len == 0) {
		return false;
	}

	if ((unsigned char)blob[LUAEXT_SEAL_MAGIC_LEN] != LUAEXT_SEAL_VERSION) {
		return false;
	}

	/*
	 * The algorithm the blob claims must be the one this sandbox is configured
	 * for. Verifying with whatever the blob asks for would let an authenticated
	 * blob be downgraded to a checksummed one -- recompute a hash anybody can
	 * compute, set the byte, and the key stops mattering.
	 */
	if ((unsigned char)blob[LUAEXT_SEAL_MAGIC_LEN + 1] != (unsigned char)expected) {
		return false;
	}

	if (length < LUAEXT_SEAL_PREFIX_LEN + digest_len) {
		return false;
	}

	body = blob + LUAEXT_SEAL_PREFIX_LEN + digest_len;
	body_len = length - LUAEXT_SEAL_PREFIX_LEN - digest_len;

	luaext_seal_digest(expected, key, key_len, body, body_len, computed);

	if (!luaext_seal_equal(computed, (const unsigned char *)blob + LUAEXT_SEAL_PREFIX_LEN,
						   digest_len)) {
		return false;
	}

	*payload = body;
	*payload_len = body_len;

	return true;
}
