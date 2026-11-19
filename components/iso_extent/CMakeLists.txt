if(COMMAND idf_component_register)
    # ESP-IDF build
    idf_component_register(SRCS "iso_extent.c"
                           INCLUDE_DIRS "include"
                           REQUIRES iso_seams)
else()
    # Host build (CMake direct)
    add_library(iso_extent STATIC iso_extent.c)
    target_include_directories(iso_extent PUBLIC include ${CMAKE_CURRENT_LIST_DIR}/../iso_seams/include)

    # Enforce C99, ASan, UBSan, and warnings
    target_compile_options(iso_extent PRIVATE
        -std=c99
        -Wall -Wextra -Werror
        -fsanitize=address -fsanitize=undefined
    )
    target_link_options(iso_extent PRIVATE
        -fsanitize=address -fsanitize=undefined
    )
endif()
