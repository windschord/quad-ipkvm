if (NOT COMMAND idf_component_register)
    # Host tests build only the pure-C logic (see test/host/CMakeLists.txt).
    add_library(csi_video_source_logic STATIC csi_video_source_logic.c)
    target_include_directories(csi_video_source_logic PUBLIC include)
    target_link_libraries(csi_video_source_logic PUBLIC kvm_logic)
else()
    idf_component_register(
        SRCS "csi_video_source.c" "csi_video_source_logic.c"
        INCLUDE_DIRS "include"
        REQUIRES frame_hub kvm_logic tc358743
        PRIV_REQUIRES csi_capture esp_timer esp_system freertos
    )
endif()
