if(COMMAND idf_component_register)
    idf_component_register(
        SRCS "iso_worker_logic.c" "iso_worker.c"
        INCLUDE_DIRS "include"
        REQUIRES iso_seams iso_extent iso_catalog iso_upload iso_core iso_media iso_sd
        PRIV_REQUIRES mbedtls esp_hw_support esp_psram freertos log
    )
else()
    add_library(iso_worker_logic STATIC iso_worker_logic.c)
    target_include_directories(iso_worker_logic PUBLIC
        include
        ${CMAKE_CURRENT_LIST_DIR}/../iso_seams/include
    )
    target_link_libraries(iso_worker_logic PUBLIC iso_core iso_media)
    set_target_properties(iso_worker_logic PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    target_compile_options(iso_worker_logic PRIVATE
        -Wall -Wextra -Werror
        -fsanitize=address -fsanitize=undefined
        -fno-sanitize-recover=undefined
    )
    target_link_options(iso_worker_logic INTERFACE
        -fsanitize=address -fsanitize=undefined
    )
endif()
