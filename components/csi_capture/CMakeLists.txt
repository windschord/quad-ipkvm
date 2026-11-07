if (NOT COMMAND idf_component_register)
    # Host tests build only the pure-C part (see test/host/CMakeLists.txt).
    add_library(csi_capture_logic STATIC csi_capture_logic.c)
    target_include_directories(csi_capture_logic PUBLIC include)
else()
    idf_component_register(
        SRCS "csi_capture.c" "csi_capture_logic.c" "csi_jpeg.c"
        INCLUDE_DIRS "include"
        REQUIRES esp_driver_cam
        PRIV_REQUIRES esp_driver_isp esp_driver_jpeg esp_mm esp_timer esp_hw_support soc
    )
endif()
