if(COMMAND idf_component_register)
    idf_component_register(
        SRCS "iso_sd_logic.c" "iso_sd.c"
        INCLUDE_DIRS "include"
        REQUIRES iso_seams
        PRIV_REQUIRES fatfs sdmmc esp_driver_sdmmc esp_driver_gpio esp_timer esp_hw_support esp_psram freertos log
    )
else()
    add_library(iso_sd_logic STATIC iso_sd_logic.c)
    target_include_directories(iso_sd_logic PUBLIC
        include
        ${CMAKE_CURRENT_LIST_DIR}/../iso_seams/include
    )
    set_target_properties(iso_sd_logic PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    target_compile_options(iso_sd_logic PRIVATE
        -Wall -Wextra -Werror
        -fsanitize=address -fsanitize=undefined
    )
    target_link_options(iso_sd_logic INTERFACE
        -fsanitize=address -fsanitize=undefined
    )
endif()
