#include "auth_crypto_mbedtls.h"
#include <string.h>

// ESP-IDF mbedTLS wrappers
#include "mbedtls/md.h"
#include "mbedtls/pkcs5.h"
#include "esp_random.h"

static int mbedtls_sha256_wrapper(const uint8_t *input, size_t ilen, uint8_t output[32]) {
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!info) return -1;

    int ret = mbedtls_md_setup(&ctx, info, 0);
    if (ret == 0) ret = mbedtls_md_starts(&ctx);
    if (ret == 0) ret = mbedtls_md_update(&ctx, input, ilen);
    if (ret == 0) ret = mbedtls_md_finish(&ctx, output);
    mbedtls_md_free(&ctx);
    return ret;
}

static int mbedtls_hmac_sha256_wrapper(const uint8_t *key, size_t keylen, const uint8_t *input, size_t ilen, uint8_t output[32]) {
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!info) return -1;

    int ret = mbedtls_md_setup(&ctx, info, 1); // 1 = HMAC
    if (ret == 0) ret = mbedtls_md_hmac_starts(&ctx, key, keylen);
    if (ret == 0) ret = mbedtls_md_hmac_update(&ctx, input, ilen);
    if (ret == 0) ret = mbedtls_md_hmac_finish(&ctx, output);
    mbedtls_md_free(&ctx);
    return ret;
}

static int mbedtls_pbkdf2_wrapper(const uint8_t *password, size_t plen, const uint8_t *salt, size_t slen, uint32_t iteration_count, uint32_t key_length, uint8_t *output) {
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!info) return -1;

    int ret = mbedtls_md_setup(&ctx, info, 1);
    if (ret == 0) {
        ret = mbedtls_pkcs5_pbkdf2_hmac(&ctx, password, plen, salt, slen, iteration_count, key_length, output);
    }
    mbedtls_md_free(&ctx);
    return ret;
}

static int mbedtls_random_wrapper(uint8_t *output, size_t len) {
    // esp_fill_random generates cryptographic random numbers from hardware RNG
    esp_fill_random(output, len);
    return 0; // Always succeeds
}

static int mbedtls_const_time_memcmp(const void *a, const void *b, size_t len) {
    const uint8_t *p1 = (const uint8_t *)a;
    const uint8_t *p2 = (const uint8_t *)b;
    uint8_t res = 0;
    for (size_t i = 0; i < len; i++) {
        res |= (p1[i] ^ p2[i]);
    }
    return res == 0 ? 0 : -1;
}

static const auth_crypto_if_t mbedtls_if = {
    .sha256 = mbedtls_sha256_wrapper,
    .hmac_sha256 = mbedtls_hmac_sha256_wrapper,
    .pbkdf2_hmac_sha256 = mbedtls_pbkdf2_wrapper,
    .random = mbedtls_random_wrapper,
    .const_time_memcmp = mbedtls_const_time_memcmp
};

const auth_crypto_if_t* auth_crypto_mbedtls_get_interface(void) {
    return &mbedtls_if;
}
