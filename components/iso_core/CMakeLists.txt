if(COMMAND idf_component_register)
    # ESP-IDF build
    idf_component_register(SRCS "iso_core.c"
                           INCLUDE_DIRS "include"
                           REQUIRES iso_seams iso_extent iso_catalog iso_upload)
else()
    # Host build (CMake direct)
    add_library(iso_core STATIC iso_core.c)
    target_include_directories(iso_core PUBLIC include ${CMAKE_CURRENT_LIST_DIR}/../iso_seams/include)
    target_link_libraries(iso_core PUBLIC iso_upload)

    # Enforce C11, ASan, UBSan, and warnings
    target_compile_options(iso_core PRIVATE
        -std=c11
        -Wall -Wextra -Werror
        -fsanitize=address -fsanitize=undefined
        -fno-sanitize-recover=undefined # UBSan の検出で、テストを失敗させる(変異テストの検出にも使う)
    )
    target_link_options(iso_core PRIVATE
        -fsanitize=address -fsanitize=undefined
    )
endif()
