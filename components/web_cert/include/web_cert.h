#pragma once

#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Load certificate and private key from NVS, or create them if they do not exist.
 *
 * It will fail-closed if creation fails, returning an error.
 * It expects psa_crypto_init() to have been called.
 *
 * @param[out] cert_pem Pointer to the loaded or created certificate in PEM format. Will be allocated and null-terminated.
 * @param[out] cert_len Length of the certificate PEM (including null terminator).
 * @param[out] key_pem Pointer to the loaded or created private key in PEM format. Will be allocated and null-terminated.
 * @param[out] key_len Length of the private key PEM (including null terminator).
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t web_cert_load_or_create(char **cert_pem, size_t *cert_len, char **key_pem, size_t *key_len);

#ifdef __cplusplus
}
#endif
