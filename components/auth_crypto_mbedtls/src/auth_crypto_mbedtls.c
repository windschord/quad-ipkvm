#include "auth_crypto_mbedtls.h"
#include <string.h>

// ESP-IDF PSA Crypto wrappers
#include "psa/crypto.h"

static bool initialized = false;

static void ensure_psa_init(void) {
    if (!initialized) {
        psa_crypto_init(); // idempotent, but good practice
        initialized = true;
    }
}

static int mbedtls_sha256_wrapper(const uint8_t *input, size_t ilen, uint8_t output[32]) {
    ensure_psa_init();
    size_t hash_length;
    psa_status_t status = psa_hash_compute(PSA_ALG_SHA_256, input, ilen, output, 32, &hash_length);
    return (status == PSA_SUCCESS && hash_length == 32) ? 0 : -1;
}

static int mbedtls_hmac_sha256_wrapper(const uint8_t *key, size_t keylen, const uint8_t *input, size_t ilen, uint8_t output[32]) {
    ensure_psa_init();

    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_SIGN_MESSAGE);
    psa_set_key_algorithm(&attributes, PSA_ALG_HMAC(PSA_ALG_SHA_256));
    psa_set_key_type(&attributes, PSA_KEY_TYPE_HMAC);

    psa_key_id_t key_id = 0;
    psa_status_t status = psa_import_key(&attributes, key, keylen, &key_id);
    if (status != PSA_SUCCESS) return -1;

    size_t mac_length;
    status = psa_mac_compute(key_id, PSA_ALG_HMAC(PSA_ALG_SHA_256), input, ilen, output, 32, &mac_length);

    psa_destroy_key(key_id);
    return (status == PSA_SUCCESS && mac_length == 32) ? 0 : -1;
}

static int mbedtls_pbkdf2_wrapper(const uint8_t *password, size_t plen, const uint8_t *salt, size_t slen, uint32_t iteration_count, uint32_t key_length, uint8_t *output) {
    ensure_psa_init();

    psa_key_derivation_operation_t op = PSA_KEY_DERIVATION_OPERATION_INIT;
    psa_status_t status = psa_key_derivation_setup(&op, PSA_ALG_PBKDF2_HMAC(PSA_ALG_SHA_256));
    if (status != PSA_SUCCESS) goto cleanup;

    status = psa_key_derivation_input_integer(&op, PSA_KEY_DERIVATION_INPUT_COST, iteration_count);
    if (status != PSA_SUCCESS) goto cleanup;

    status = psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_SALT, salt, slen);
    if (status != PSA_SUCCESS) goto cleanup;

    status = psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_PASSWORD, password, plen);
    if (status != PSA_SUCCESS) goto cleanup;

    status = psa_key_derivation_output_bytes(&op, output, key_length);

cleanup:
    psa_key_derivation_abort(&op);
    return (status == PSA_SUCCESS) ? 0 : -1;
}

static int mbedtls_random_wrapper(uint8_t *output, size_t len) {
    ensure_psa_init();
    psa_status_t status = psa_generate_random(output, len);
    return (status == PSA_SUCCESS) ? 0 : -1;
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
