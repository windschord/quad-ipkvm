if (NOT COMMAND idf_component_register)
    # We are in host test environment
    add_library(ws_video_core STATIC "ws_video_core.c")
    target_include_directories(ws_video_core PUBLIC "include")
    target_link_libraries(ws_video_core PUBLIC ws_clients frame_hub)
else()
    # We are in ESP-IDF build
    idf_component_register(
        SRCS "ws_video_core.c"
        INCLUDE_DIRS "include"
        REQUIRES ws_clients frame_hub
    )
endif()
