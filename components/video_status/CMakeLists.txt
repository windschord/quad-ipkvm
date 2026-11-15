if(COMMAND idf_component_register)
    # ESP-IDF build
    idf_component_register(SRCS "video_status.c"
                           INCLUDE_DIRS "include"
                           REQUIRES kvm_logic)
else()
    # Host build (CMake direct)
    add_library(video_status video_status.c)
    target_include_directories(video_status PUBLIC include)
    target_link_libraries(video_status PUBLIC kvm_logic)

    # Enforce C99, ASan, UBSan, and warnings
    target_compile_options(video_status PRIVATE
        -std=c99
        -Wall -Wextra -Werror
        -fsanitize=address -fsanitize=undefined
    )
    target_link_options(video_status PRIVATE
        -fsanitize=address -fsanitize=undefined
    )
endif()
