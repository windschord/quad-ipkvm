idf_component_register(
    SRCS "src/auth_crypto_mbedtls.c"
    INCLUDE_DIRS "include"
    REQUIRES "mbedtls" "esp_hw_support" "auth_logic"
)
