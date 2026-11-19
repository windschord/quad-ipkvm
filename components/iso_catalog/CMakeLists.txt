if(COMMAND idf_component_register)
    # ESP-IDF build
    idf_component_register(SRCS "iso_catalog.c" "iso_meta.c" "iso_name.c"
                           INCLUDE_DIRS "include"
                           REQUIRES iso_seams iso_extent)
else()
    # Host build (CMake direct)
    add_library(iso_catalog STATIC iso_catalog.c iso_meta.c iso_name.c)
    target_include_directories(iso_catalog PUBLIC include ${CMAKE_CURRENT_LIST_DIR}/../iso_seams/include)
    target_link_libraries(iso_catalog PUBLIC iso_extent)

    # Enforce C99, ASan, UBSan, and warnings
    target_compile_options(iso_catalog PRIVATE
        -std=c99
        -Wall -Wextra -Werror
        -fsanitize=address -fsanitize=undefined
    )
    target_link_options(iso_catalog PRIVATE
        -fsanitize=address -fsanitize=undefined
    )
endif()
