if(COMMAND idf_component_register)
    # ESP-IDF build
    idf_component_register(SRCS "iso_upload.c" "iso_finish.c" "iso_index.c"
                           INCLUDE_DIRS "include"
                           REQUIRES iso_seams iso_extent iso_catalog)
else()
    # Host build (CMake direct)
    add_library(iso_upload STATIC iso_upload.c iso_finish.c iso_index.c)
    target_include_directories(iso_upload PUBLIC include ${CMAKE_CURRENT_LIST_DIR}/../iso_seams/include)
    target_link_libraries(iso_upload PUBLIC iso_catalog iso_extent)

    # Enforce C11, ASan, UBSan, and warnings
    target_compile_options(iso_upload PRIVATE
        -std=c11
        -Wall -Wextra -Werror
        -fsanitize=address -fsanitize=undefined
    )
    target_link_options(iso_upload PRIVATE
        -fsanitize=address -fsanitize=undefined
    )
endif()
