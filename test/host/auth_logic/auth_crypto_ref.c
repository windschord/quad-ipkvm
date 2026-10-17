#include "../../../components/auth_logic/include/auth_logic.h"
#include <string.h>
#include <stdlib.h>

// --- SHA-256 ---
#define SHA256_BLOCK_SIZE 64

static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

#define ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
#define CH(x, y, z) (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define EP0(x) (ROTR(x, 2) ^ ROTR(x, 13) ^ ROTR(x, 22))
#define EP1(x) (ROTR(x, 6) ^ ROTR(x, 11) ^ ROTR(x, 25))
#define SIG0(x) (ROTR(x, 7) ^ ROTR(x, 18) ^ ((x) >> 3))
#define SIG1(x) (ROTR(x, 17) ^ ROTR(x, 19) ^ ((x) >> 10))

typedef struct {
    uint32_t state[8];
    uint64_t count;
    uint8_t buffer[64];
} sha256_ctx;

static void sha256_transform(sha256_ctx *ctx, const uint8_t *data) {
    uint32_t a, b, c, d, e, f, g, h, i, j, t1, t2, m[64];

    for (i = 0, j = 0; i < 16; ++i, j += 4)
        m[i] = (data[j] << 24) | (data[j + 1] << 16) | (data[j + 2] << 8) | (data[j + 3]);
    for (; i < 64; ++i)
        m[i] = SIG1(m[i - 2]) + m[i - 7] + SIG0(m[i - 15]) + m[i - 16];

    a = ctx->state[0];
    b = ctx->state[1];
    c = ctx->state[2];
    d = ctx->state[3];
    e = ctx->state[4];
    f = ctx->state[5];
    g = ctx->state[6];
    h = ctx->state[7];

    for (i = 0; i < 64; ++i) {
        t1 = h + EP1(e) + CH(e, f, g) + K[i] + m[i];
        t2 = EP0(a) + MAJ(a, b, c);
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }

    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;
    ctx->state[4] += e;
    ctx->state[5] += f;
    ctx->state[6] += g;
    ctx->state[7] += h;
}

static void sha256_init(sha256_ctx *ctx) {
    ctx->state[0] = 0x6a09e667;
    ctx->state[1] = 0xbb67ae85;
    ctx->state[2] = 0x3c6ef372;
    ctx->state[3] = 0xa54ff53a;
    ctx->state[4] = 0x510e527f;
    ctx->state[5] = 0x9b05688c;
    ctx->state[6] = 0x1f83d9ab;
    ctx->state[7] = 0x5be0cd19;
    ctx->count = 0;
}

static void sha256_update(sha256_ctx *ctx, const uint8_t *data, size_t len) {
    size_t i = (size_t)(ctx->count % 64);
    ctx->count += len;
    for (size_t j = 0; j < len; ++j) {
        ctx->buffer[i++] = data[j];
        if (i == 64) {
            sha256_transform(ctx, ctx->buffer);
            i = 0;
        }
    }
}

static void sha256_final(sha256_ctx *ctx, uint8_t hash[32]) {
    uint64_t bitlen = ctx->count * 8;
    size_t i = (size_t)(ctx->count % 64);
    ctx->buffer[i++] = 0x80;
    if (i > 56) {
        while (i < 64) ctx->buffer[i++] = 0;
        sha256_transform(ctx, ctx->buffer);
        i = 0;
    }
    while (i < 56) ctx->buffer[i++] = 0;
    for (i = 0; i < 8; ++i) {
        ctx->buffer[63 - i] = (uint8_t)(bitlen >> (i * 8));
    }
    sha256_transform(ctx, ctx->buffer);
    for (i = 0; i < 8; ++i) {
        for (int j = 0; j < 4; ++j) {
            hash[i * 4 + j] = (uint8_t)(ctx->state[i] >> (24 - j * 8));
        }
    }
}

static int ref_sha256(const uint8_t *input, size_t ilen, uint8_t output[32]) {
    sha256_ctx ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, input, ilen);
    sha256_final(&ctx, output);
    return 0;
}

static int ref_hmac_sha256(const uint8_t *key, size_t keylen, const uint8_t *input, size_t ilen, uint8_t output[32]) {
    uint8_t k_ipad[64];
    uint8_t k_opad[64];
    uint8_t tk[32];
    const uint8_t *k;
    size_t kl;

    if (keylen > 64) {
        ref_sha256(key, keylen, tk);
        k = tk;
        kl = 32;
    } else {
        k = key;
        kl = keylen;
    }

    memset(k_ipad, 0x36, 64);
    memset(k_opad, 0x5c, 64);

    for (size_t i = 0; i < kl; i++) {
        k_ipad[i] ^= k[i];
        k_opad[i] ^= k[i];
    }

    sha256_ctx ctx;
    uint8_t inner_hash[32];
    sha256_init(&ctx);
    sha256_update(&ctx, k_ipad, 64);
    sha256_update(&ctx, input, ilen);
    sha256_final(&ctx, inner_hash);

    sha256_init(&ctx);
    sha256_update(&ctx, k_opad, 64);
    sha256_update(&ctx, inner_hash, 32);
    sha256_final(&ctx, output);

    return 0;
}

static int ref_pbkdf2_hmac_sha256(const uint8_t *password, size_t plen, const uint8_t *salt, size_t slen, uint32_t iteration_count, uint32_t key_length, uint8_t *output) {
    uint32_t i, j, k;
    uint8_t U[32];
    uint8_t T[32];
    uint8_t block[256 + 4]; // Salt + counter
    if (slen > 256) return -1; // Keep it simple for ref

    memcpy(block, salt, slen);

    uint32_t blocks_needed = (key_length + 31) / 32;
    size_t out_len = 0;

    for (i = 1; i <= blocks_needed; i++) {
        block[slen] = (i >> 24) & 0xFF;
        block[slen + 1] = (i >> 16) & 0xFF;
        block[slen + 2] = (i >> 8) & 0xFF;
        block[slen + 3] = i & 0xFF;

        ref_hmac_sha256(password, plen, block, slen + 4, U);
        memcpy(T, U, 32);

        for (j = 1; j < iteration_count; j++) {
            ref_hmac_sha256(password, plen, U, 32, U);
            for (k = 0; k < 32; k++) {
                T[k] ^= U[k];
            }
        }

        size_t copy_len = (key_length - out_len) < 32 ? (key_length - out_len) : 32;
        memcpy(output + out_len, T, copy_len);
        out_len += copy_len;
    }

    return 0;
}

// Global flag to inject random failures during testing
bool g_ref_random_should_fail = false;

static int ref_random(uint8_t *output, size_t len) {
    if (g_ref_random_should_fail) return -1;
    // Just simple pseudo-random for tests, not secure!
    for (size_t i = 0; i < len; i++) {
        output[i] = rand() & 0xFF;
    }
    return 0;
}

static int ref_const_time_memcmp(const void *a, const void *b, size_t len) {
    const uint8_t *p1 = (const uint8_t *)a;
    const uint8_t *p2 = (const uint8_t *)b;
    uint8_t res = 0;
    for (size_t i = 0; i < len; i++) {
        res |= (p1[i] ^ p2[i]);
    }
    return res == 0 ? 0 : -1;
}

const auth_crypto_if_t ref_crypto_if = {
    .sha256 = ref_sha256,
    .hmac_sha256 = ref_hmac_sha256,
    .pbkdf2_hmac_sha256 = ref_pbkdf2_hmac_sha256,
    .random = ref_random,
    .const_time_memcmp = ref_const_time_memcmp
};
