if(COMMAND idf_component_register)
    # ESP-IDF build
    idf_component_register(SRCS "video_codec_policy.c"
                           INCLUDE_DIRS "include")
else()
    # Host build (CMake direct)
    add_library(video_codec_policy video_codec_policy.c)
    target_include_directories(video_codec_policy PUBLIC include)

    # Enforce C99, ASan, UBSan, and warnings
    target_compile_options(video_codec_policy PRIVATE
        -std=c99
        -Wall -Wextra -Werror
        -fsanitize=address -fsanitize=undefined
    )
    target_link_options(video_codec_policy PRIVATE
        -fsanitize=address -fsanitize=undefined
    )
endif()
