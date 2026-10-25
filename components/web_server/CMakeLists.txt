if(NOT COMMAND idf_component_register)
    # Host test for web_status_map only
    add_library(web_status_map STATIC "web_status_map.c" "web_assets.c")
    target_include_directories(web_status_map PUBLIC "include" "../auth_logic/include" "../http_guard/include")
    target_link_libraries(web_status_map PUBLIC auth_logic)
else()
    idf_component_register(
        SRCS "web_server.c" "web_status_map.c" "web_assets.c"
        INCLUDE_DIRS "include"
        REQUIRES esp_https_server nvs_flash mbedtls esp_timer web_cert auth_logic auth_crypto_mbedtls http_guard esp_hw_support net_eth lwip
        EMBED_FILES "web/index.html" "web/app.js" "web/app.css"
    )
endif()
