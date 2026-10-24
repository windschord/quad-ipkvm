#include "web_cert.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/pk.h"
#include "mbedtls/md.h"
#include "psa/crypto.h"
#include "esp_timer.h"
#include <string.h>

static const char *TAG = "web_cert";

static esp_err_t save_to_nvs(const char *cert_pem, const char *key_pem) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open("web_cert", NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    err = nvs_set_str(handle, "cert", cert_pem);
    if (err == ESP_OK) {
        err = nvs_set_str(handle, "key", key_pem);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

static esp_err_t load_from_nvs(char **cert_pem, size_t *cert_len, char **key_pem, size_t *key_len) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open("web_cert", NVS_READONLY, &handle);
    if (err != ESP_OK) return err;

    err = nvs_get_str(handle, "cert", NULL, cert_len);
    if (err != ESP_OK) goto end;

    *cert_pem = malloc(*cert_len);
    if (!*cert_pem) { err = ESP_ERR_NO_MEM; goto end; }

    err = nvs_get_str(handle, "cert", *cert_pem, cert_len);
    if (err != ESP_OK) goto end;

    err = nvs_get_str(handle, "key", NULL, key_len);
    if (err != ESP_OK) goto end;

    *key_pem = malloc(*key_len);
    if (!*key_pem) { err = ESP_ERR_NO_MEM; goto end; }

    err = nvs_get_str(handle, "key", *key_pem, key_len);

end:
    nvs_close(handle);
    if (err != ESP_OK) {
        if (*cert_pem) { free(*cert_pem); *cert_pem = NULL; }
        if (*key_pem) { free(*key_pem); *key_pem = NULL; }
    }
    return err;
}

esp_err_t web_cert_load_or_create(char **cert_pem, size_t *cert_len, char **key_pem, size_t *key_len) {
    if (!cert_pem || !cert_len || !key_pem || !key_len) return ESP_ERR_INVALID_ARG;

    *cert_pem = NULL;
    *key_pem = NULL;

    if (load_from_nvs(cert_pem, cert_len, key_pem, key_len) == ESP_OK) {
        ESP_LOGI(TAG, "Loaded certificate and key from NVS.");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Certificate not found or could not be loaded. Generating new one...");

    psa_crypto_init();
    // Timing start
    uint64_t start = esp_timer_get_time();

    mbedtls_svc_key_id_t key_id;
    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_SIGN_HASH | PSA_KEY_USAGE_EXPORT);
    psa_set_key_algorithm(&attributes, PSA_ALG_ECDSA(PSA_ALG_SHA_256));
    psa_set_key_type(&attributes, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_bits(&attributes, 256);

    psa_status_t status = psa_generate_key(&attributes, &key_id);
    if (status != PSA_SUCCESS) {
        ESP_LOGE(TAG, "Failed to generate PSA key: %d", status);
        return ESP_FAIL;
    }

    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    if (mbedtls_pk_copy_from_psa(key_id, &pk) != 0) {
        ESP_LOGE(TAG, "Failed to copy key to PK context");
        psa_destroy_key(key_id);
        mbedtls_pk_free(&pk);
        return ESP_FAIL;
    }

    psa_destroy_key(key_id); // No longer needed

    mbedtls_x509write_cert crt;
    mbedtls_x509write_crt_init(&crt);

    mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);

    mbedtls_x509write_crt_set_subject_key(&crt, &pk);
    mbedtls_x509write_crt_set_issuer_key(&crt, &pk);

    if (mbedtls_x509write_crt_set_subject_name(&crt, "CN=quad-ipkvm") != 0) {
         ESP_LOGE(TAG, "Failed to set subject name");
         goto err_cleanup;
    }
    if (mbedtls_x509write_crt_set_issuer_name(&crt, "CN=quad-ipkvm") != 0) {
         ESP_LOGE(TAG, "Failed to set issuer name");
         goto err_cleanup;
    }

    // 10 years validity
    if (mbedtls_x509write_crt_set_validity(&crt, "20230101000000", "20330101000000") != 0) {
         ESP_LOGE(TAG, "Failed to set validity");
         goto err_cleanup;
    }

    // Generate random serial
    unsigned char serial[16];
    psa_generate_random(serial, sizeof(serial));
    if (mbedtls_x509write_crt_set_serial_raw(&crt, serial, sizeof(serial)) != 0) {
         ESP_LOGE(TAG, "Failed to set serial");
         goto err_cleanup;
    }

    if (mbedtls_x509write_crt_set_extension(&crt, MBEDTLS_OID_SUBJECT_ALT_NAME, MBEDTLS_OID_SIZE(MBEDTLS_OID_SUBJECT_ALT_NAME), 0, (const unsigned char*)"\x82\x10""quad-ipkvm.local", 18) != 0) {
         ESP_LOGE(TAG, "Failed to set SAN");
         goto err_cleanup;
    }

    // Write Cert
    *cert_len = 2048;
    *cert_pem = malloc(*cert_len);
    if (!*cert_pem) goto err_cleanup;

    if (mbedtls_x509write_crt_pem(&crt, (unsigned char *)*cert_pem, *cert_len) != 0) {
         ESP_LOGE(TAG, "Failed to write cert to PEM");
         goto err_cleanup;
    }
    *cert_len = strlen(*cert_pem) + 1; // Include null terminator

    // Write Key
    *key_len = 2048;
    *key_pem = malloc(*key_len);
    if (!*key_pem) goto err_cleanup;

    if (mbedtls_pk_write_key_pem(&pk, (unsigned char *)*key_pem, *key_len) != 0) {
        ESP_LOGE(TAG, "Failed to write key to PEM");
        goto err_cleanup;
    }
    *key_len = strlen(*key_pem) + 1;

    mbedtls_x509write_crt_free(&crt);
    mbedtls_pk_free(&pk);

    uint64_t duration = (esp_timer_get_time() - start) / 1000;
    ESP_LOGI(TAG, "Certificate generation took %llu ms", (unsigned long long)duration);

    // Verify pair
    mbedtls_x509_crt parse_crt;
    mbedtls_x509_crt_init(&parse_crt);
    mbedtls_pk_context parse_pk;
    mbedtls_pk_init(&parse_pk);

    if (mbedtls_x509_crt_parse(&parse_crt, (const unsigned char*)*cert_pem, *cert_len) != 0) {
        ESP_LOGE(TAG, "Failed to verify parse generated cert");
        mbedtls_x509_crt_free(&parse_crt);
        mbedtls_pk_free(&parse_pk);
        goto final_err;
    }

    if (mbedtls_pk_parse_key(&parse_pk, (const unsigned char*)*key_pem, *key_len, NULL, 0) != 0) {
        ESP_LOGE(TAG, "Failed to verify parse generated key");
        mbedtls_x509_crt_free(&parse_crt);
        mbedtls_pk_free(&parse_pk);
        goto final_err;
    }

    if (mbedtls_pk_check_pair(&parse_crt.pk, &parse_pk) != 0) {
        ESP_LOGE(TAG, "Failed to verify cert/key pair");
        mbedtls_x509_crt_free(&parse_crt);
        mbedtls_pk_free(&parse_pk);
        goto final_err;
    }

    mbedtls_x509_crt_free(&parse_crt);
    mbedtls_pk_free(&parse_pk);

    if (save_to_nvs(*cert_pem, *key_pem) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save generated cert/key to NVS");
        goto final_err;
    }

    return ESP_OK;

err_cleanup:
    mbedtls_x509write_crt_free(&crt);
    mbedtls_pk_free(&pk);
final_err:
    if (*cert_pem) { free(*cert_pem); *cert_pem = NULL; }
    if (*key_pem) { free(*key_pem); *key_pem = NULL; }
    return ESP_FAIL;
}
