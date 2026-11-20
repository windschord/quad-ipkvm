if (NOT COMMAND idf_component_register)
    # Host tests build only the pure-C logic (see test/host/CMakeLists.txt).
    add_library(csi_h264_logic STATIC
        csi_h264_logic.c
    )
    target_include_directories(csi_h264_logic PUBLIC include)
else()
    set(priv_reqs esp_driver_ppa esp_mm esp_hw_support esp_timer)
    if(CONFIG_CSI_H264_ENABLE)
        list(APPEND priv_reqs esp_h264)
    endif()
    idf_component_register(
        SRCS "csi_h264.c" "csi_h264_logic.c"
        INCLUDE_DIRS "include"
        PRIV_REQUIRES ${priv_reqs}
    )
endif()
