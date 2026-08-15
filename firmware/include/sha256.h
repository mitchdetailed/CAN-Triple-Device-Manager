/* SHA-256 and HMAC-SHA256 — the only cryptographic primitive the device needs.
 *
 * The configuration password protocol is deliberately built on this one
 * function (see src/model/config_lock.h on the PC side): the device never
 * decrypts anything, it only ever answers "is this the right password", so it
 * needs a hash and nothing else. No block cipher, no key derivation — the PC
 * does the expensive PBKDF2 stretching and the device only ever handles the
 * 32-byte verifier that falls out of it.
 *
 * Verified against the FIPS 180-4 and RFC 4231 vectors in test_firmware_link,
 * which compiles this file for the host.
 */
#ifndef SHA256_H
#define SHA256_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SHA256_DIGEST_LEN 32u
#define SHA256_BLOCK_LEN 64u

typedef struct {
    uint32_t state[8];
    uint64_t bit_count;
    uint8_t buffer[SHA256_BLOCK_LEN];
    uint32_t buffered;
} Sha256Ctx;

void sha256_init(Sha256Ctx *ctx);
void sha256_update(Sha256Ctx *ctx, const uint8_t *data, uint32_t length);
void sha256_final(Sha256Ctx *ctx, uint8_t out[SHA256_DIGEST_LEN]);

/* One-shot convenience. */
void sha256(const uint8_t *data, uint32_t length, uint8_t out[SHA256_DIGEST_LEN]);

/* HMAC-SHA256 (RFC 2104). Keys longer than a block are hashed first. */
void hmac_sha256(const uint8_t *key, uint32_t key_len, const uint8_t *data, uint32_t data_len,
                 uint8_t out[SHA256_DIGEST_LEN]);

/* Length-independent equality, for comparing secrets. Never short-circuits, so
 * a timing measurement cannot walk a value out one byte at a time. */
bool sha256_equal_ct(const uint8_t *a, const uint8_t *b, uint32_t length);

#ifdef __cplusplus
}
#endif

#endif /* SHA256_H */
