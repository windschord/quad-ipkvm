if(COMMAND idf_component_register)
    # ESP-IDF build
    idf_component_register(SRCS "iso_media.c"
                           INCLUDE_DIRS "include"
                           REQUIRES "")
else()
    # Host build (CMake direct)
    add_library(iso_media STATIC iso_media.c)
    target_include_directories(iso_media PUBLIC include)

    # Enforce C99, ASan, UBSan, and warnings
    target_compile_options(iso_media PRIVATE
        -std=c99
        -Wall -Wextra -Werror
        -fsanitize=address -fsanitize=undefined
    )
    target_link_options(iso_media PRIVATE
        -fsanitize=address -fsanitize=undefined
    )
endif()
