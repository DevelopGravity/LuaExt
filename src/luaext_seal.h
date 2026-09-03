/*
 * luaext — authenticating bytecode before the loader sees it.
 *
 * Lua's binary loader validates the header, the buffer bounds, constant tags
 * and string indices. It does NOT validate opcodes, register indices, constant
 * indices or jump targets, so corruption in the instruction stream goes
 * straight into the VM. Measured on a 118-byte chunk by flipping one byte at
 * each position: 57% refused, 33% RAN ANYWAY, and 10% killed the process.
 *
 * There is no verifier to add -- Lua has never had one and writing one is not a
 * realistic undertaking -- so the only sound move is to refuse bytes we cannot
 * vouch for. A blob carries an HMAC-SHA256 taken with a key the host supplies,
 * and it is checked before a byte reaches luaL_loadbufferx.
 *
 * WHAT THIS BUYS, precisely: it closes accidental corruption and tampering by
 * anyone without the key. It does NOT survive host compromise -- an attacker
 * who can read process memory has the key -- and it says nothing about whether
 * the bytecode was safe to begin with. It authenticates ORIGIN, not SAFETY.
 */

#ifndef LUAEXT_SEAL_H
#define LUAEXT_SEAL_H

#include "luaext_types.h"

/*
 * Wire format. 37 bytes of overhead:
 *
 *   "LXBC"     4   magic; a raw Lua chunk starts "\x1bLua", so the two forms
 *                  are distinguishable without guessing
 *   version    1   currently 1
 *   mac       32   HMAC-SHA256(key, version || payload)
 *   payload    n   the binary chunk exactly as lua_dump produced it
 *
 * The MAC covers the version byte so a future format cannot be downgraded by
 * rewriting it. It deliberately does not cover the chunk name: that is supplied
 * at load time and only affects tracebacks, so binding it would stop one blob
 * being loaded under a different name and buy nothing.
 */
#define LUAEXT_SEAL_MAGIC "LXBC"
#define LUAEXT_SEAL_MAGIC_LEN 4
#define LUAEXT_SEAL_VERSION 1
#define LUAEXT_SEAL_MAC_LEN 32
#define LUAEXT_SEAL_HEADER_LEN (LUAEXT_SEAL_MAGIC_LEN + 1 + LUAEXT_SEAL_MAC_LEN)

/* Shorter than this is not a key. Refused at construction rather than quietly
 * accepted, because a two-byte key authenticates nothing while looking as
 * though it does. */
#define LUAEXT_SEAL_MIN_KEY_LEN 16

/*
 * Resolve the SHA-256 implementation. Call once from MINIT, before anything
 * seals or verifies.
 */
void luaext_seal_startup(void);

/* Whether `blob` claims to be sealed. Says nothing about whether it verifies. */
bool luaext_seal_is_sealed(const char *blob, size_t length);

/*
 * Wrap `payload` for `key`. Returns a new zend_string the caller owns.
 */
zend_string *luaext_seal_wrap(const char *payload, size_t payload_len, const char *key,
							  size_t key_len);

/*
 * Verify `blob` against `key` and report where the payload starts.
 *
 * Returns false without touching the outputs if the blob is malformed, the
 * version is unknown, or the MAC does not match. The caller must not look at
 * the payload of a blob this rejected.
 */
bool luaext_seal_open(const char *blob, size_t length, const char *key, size_t key_len,
					  const char **payload, size_t *payload_len);

#endif /* LUAEXT_SEAL_H */
