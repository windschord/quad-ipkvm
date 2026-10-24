idf_component_register(SRCS "web_server.c" INCLUDE_DIRS "include" REQUIRES esp_https_server nvs_flash mbedtls esp_timer web_cert auth_logic auth_crypto_mbedtls http_guard esp_hw_support net_eth lwip)
