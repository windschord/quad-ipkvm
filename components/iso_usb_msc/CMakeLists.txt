if(COMMAND idf_component_register)
    # ESP-IDF build
    idf_component_register(
        SRCS "iso_usb_msc_glue.c" "iso_usb_msc.c"
        INCLUDE_DIRS "include"
        REQUIRES iso_seams iso_extent iso_media mmc_scsi msc_bot
        PRIV_REQUIRES esp_tinyusb freertos esp_hw_support log
    )
else()
    # Host build (pure C11 glue; no ESP-IDF/TinyUSB/FreeRTOS deps)
    add_library(iso_usb_msc_glue STATIC iso_usb_msc_glue.c)
    target_include_directories(iso_usb_msc_glue PUBLIC
        include
        ${CMAKE_CURRENT_LIST_DIR}/../iso_seams/include
    )
    target_link_libraries(iso_usb_msc_glue PUBLIC
        iso_extent
        iso_media
        mmc_scsi
        msc_bot
    )
    # Use C_STANDARD property because parent CMake sets CMAKE_C_STANDARD=99
    set_target_properties(iso_usb_msc_glue PROPERTIES
        C_STANDARD 11
        C_STANDARD_REQUIRED ON
        C_EXTENSIONS OFF
    )
    target_compile_options(iso_usb_msc_glue PRIVATE
        -Wall -Wextra -Werror
        -fsanitize=address -fsanitize=undefined
    )
    target_link_options(iso_usb_msc_glue INTERFACE
        -fsanitize=address -fsanitize=undefined
    )
endif()
