// Self-contained SHA-256 (FIPS 180-4), used for the build-lock check: the
// carrier hashes the game exe on disk (the plaintext image — verified in
// docs/initial_analysis.md) and refuses to patch anything unless it matches
// the RE'd binary.
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t state[8];
    uint64_t total_len;   // total message length in bytes
    uint8_t buffer[64];
    size_t buffer_len;
} sha256_ctx;

void sha256_init(sha256_ctx *ctx);
void sha256_update(sha256_ctx *ctx, const void *data, size_t len);
void sha256_final(sha256_ctx *ctx, uint8_t out[32]);

// Lowercase hex, writes 65 bytes (64 hex digits + NUL).
void sha256_hex(const uint8_t hash[32], char out[65]);

#ifdef __cplusplus
}
#endif
