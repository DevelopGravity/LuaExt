/*
 * luaext — vouching for bytecode before the loader sees it.
 *
 * Lua's binary loader validates the header, the buffer bounds, constant tags
 * and string indices. It does NOT validate opcodes, register indices, constant
 * indices or jump targets, so corruption in the instruction stream goes
 * straight into the VM -- and the checked fraction SHRINKS as blobs grow.
 * Measured by flipping one byte at each position: on a 150-byte chunk, 57%
 * refused, 23% ran with the right answer anyway, 8% ran with a WRONG one, 13%
 * killed the process. On a 297 KB chunk only 17% were refused and 82% ran.
 *
 * There is no verifier to add -- Lua has never had one -- so the only sound
 * move is to refuse bytes we cannot vouch for.
 *
 * TWO WAYS TO VOUCH, because they answer different questions:
 *
 *   Checksum (xxh128, the default) answers "did this survive the trip?". It is
 *   tamper-EVIDENT: 128 bits catch corruption essentially always, and at
 *   ~12 GB/s it costs 25us on a 297 KB blob against 1219us for HMAC-SHA256.
 *   It stops nobody deliberate: anyone can recompute it.
 *
 *   Authenticated (HMAC-SHA256, with a key) additionally answers "did this come
 *   from us?". Its real value is that a blob sealed under one key will not load
 *   under another, so a bytecode store shared between processes fails CLOSED
 *   instead of silently working.
 *
 * Neither survives host compromise: an attacker who can read process memory has
 * the key. Both authenticate ORIGIN at best, never SAFETY -- the capability
 * gate and the INI are what decide whether bytecode may be loaded at all.
 */

#ifndef LUAEXT_SEAL_H
#define LUAEXT_SEAL_H

#include "luaext_types.h"

/*
 * Wire format:
 *
 *   "LXBC"     4   magic; a raw Lua chunk starts "\x1bLua", so the two forms
 *                  are distinguishable without guessing
 *   version    1   currently 1
 *   algorithm  1   luaext_seal_algo; also fixes the digest length
 *   digest     n   16 for xxh128, 32 for HMAC-SHA256
 *   payload    n   the binary chunk exactly as lua_dump produced it
 *
 * The digest covers version and algorithm as well as the payload, so neither
 * can be rewritten inside a blob that still verifies.
 *
 * The ALGORITHM BYTE IS NOT TRUSTED TO SELECT THE CHECK. A caller verifies
 * against the algorithm its sandbox is configured for and refuses anything
 * else; otherwise an authenticated blob could be downgraded to a checksummed
 * one by recomputing a hash anybody can compute.
 */
#define LUAEXT_SEAL_MAGIC "LXBC"
#define LUAEXT_SEAL_MAGIC_LEN 4
#define LUAEXT_SEAL_VERSION 1
#define LUAEXT_SEAL_PREFIX_LEN (LUAEXT_SEAL_MAGIC_LEN + 2) /* magic, version, algo */
#define LUAEXT_SEAL_MAX_DIGEST 32

typedef enum {
	LUAEXT_SEAL_CHECKSUM = 1,	  /* xxh128, unkeyed */
	LUAEXT_SEAL_AUTHENTICATED = 2 /* HMAC-SHA256, keyed */
} luaext_seal_algo;

/* Shorter than this is not a key. Refused at construction rather than quietly
 * accepted, because a two-byte key authenticates nothing while looking as
 * though it does. */
#define LUAEXT_SEAL_MIN_KEY_LEN 16

/*
 * Resolve the hash implementations. Call once from MINIT, before anything seals
 * or verifies.
 */
void luaext_seal_startup(void);

/* Whether `blob` claims to be sealed. Says nothing about whether it verifies. */
bool luaext_seal_is_sealed(const char *blob, size_t length);

/*
 * Wrap `payload` under `algo`. `key` is required for LUAEXT_SEAL_AUTHENTICATED
 * and ignored otherwise. Returns a new zend_string the caller owns.
 */
zend_string *luaext_seal_wrap(const char *payload, size_t payload_len, luaext_seal_algo algo,
							  const char *key, size_t key_len);

/*
 * Verify `blob`, which must claim `expected` as its algorithm, and report where
 * the payload starts.
 *
 * Returns false without touching the outputs if the blob is malformed, the
 * version is unknown, the algorithm is not the one asked for, or the digest
 * does not match. The caller must not look at the payload of a rejected blob.
 */
bool luaext_seal_open(const char *blob, size_t length, luaext_seal_algo expected, const char *key,
					  size_t key_len, const char **payload, size_t *payload_len);

#endif /* LUAEXT_SEAL_H */
