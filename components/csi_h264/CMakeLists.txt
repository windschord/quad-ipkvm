if (NOT COMMAND idf_component_register)
    # Host tests build only the pure-C logic (see test/host/CMakeLists.txt).
    add_library(csi_h264_logic STATIC
        csi_h264_logic.c
    )
    target_include_directories(csi_h264_logic PUBLIC include)
else()
    idf_component_register(
        SRCS "csi_h264.c" "csi_h264_logic.c"
        INCLUDE_DIRS "include"
        PRIV_REQUIRES esp_driver_ppa esp_h264 esp_mm esp_hw_support esp_timer
    )
endif()
