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

    # Enforce C11. The standard is set by the target property, not by -std=...: the parent project sets CMAKE_C_STANDARD=99,
    # and CMake adds its own -std=c99 to the command line (a hand-written -std=c11 only wins by its position).
    set_target_properties(iso_upload PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    # ASan, UBSan, and warnings
    target_compile_options(iso_upload PRIVATE
        -Wall -Wextra -Werror
        -fsanitize=address -fsanitize=undefined
        -fno-sanitize-recover=undefined # UBSan の検出で、テストを失敗させる(変異テストの検出にも使う)
    )
    target_link_options(iso_upload PRIVATE
        -fsanitize=address -fsanitize=undefined
    )
endif()
