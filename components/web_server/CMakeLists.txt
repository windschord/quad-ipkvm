if(NOT COMMAND idf_component_register)
    # Host test for web_status_map only
    add_library(web_status_map STATIC "web_status_map.c" "web_assets.c" "web_headers.c")
    target_include_directories(web_status_map PUBLIC "include" "../auth_logic/include" "../http_guard/include")
    target_link_libraries(web_status_map PUBLIC auth_logic)

    add_library(web_iso_put_logic STATIC "web_iso_put_logic.c")
    target_include_directories(web_iso_put_logic PUBLIC "include")
    set_target_properties(web_iso_put_logic PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    target_compile_options(web_iso_put_logic PRIVATE -Wall -Wextra -Werror -fsanitize=address -fsanitize=undefined -fno-sanitize-recover=undefined)
    target_link_options(web_iso_put_logic PRIVATE -fsanitize=address -fsanitize=undefined)
else()
    set(web_srcs "web_server.c" "web_status_map.c" "web_assets.c" "web_headers.c")
    set(web_requires esp_https_server nvs_flash mbedtls esp_timer web_cert auth_logic auth_crypto_mbedtls http_guard esp_hw_support net_eth lwip auth_worker_slot ws_video_core video_codec_policy video_status frame_hub ws_clients ws_input_core hid_logic kvm_api kvm_service)
    # ISO virtual media API (/api/v1/iso*, docs/WEB_API.md 8). The source is built only when ISO_ENABLE (the paths are not registered otherwise).
    # The requirements are NOT conditional: ESP-IDF expands the component requirements before sdkconfig is available, so a
    # CONFIG_ISO_ENABLE test there never adds them (the build then fails: "iso_api.h: No such file", found by CI idf-build-iso).
    # These components are pure C and are only linked when referenced.
    list(APPEND web_requires iso_api iso_core iso_media iso_upload iso_catalog iso_extent iso_seams iso_worker)
    if(CONFIG_ISO_ENABLE)
        list(APPEND web_srcs "web_iso.c" "web_iso_put_logic.c" "web_iso_service.c")
    endif()
    idf_component_register(
        SRCS ${web_srcs}
        INCLUDE_DIRS "include"
        REQUIRES ${web_requires}
        EMBED_FILES "web/index.html" "web/app.js" "web/app.css" "web/video_trust.html"
    )
endif()
