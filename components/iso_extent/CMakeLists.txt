if(COMMAND idf_component_register)
    # ESP-IDF build
    idf_component_register(SRCS "iso_extent.c"
                           INCLUDE_DIRS "include"
                           REQUIRES iso_seams)
else()
    # Host build (CMake direct)
    add_library(iso_extent STATIC iso_extent.c)
    target_include_directories(iso_extent PUBLIC include ${CMAKE_CURRENT_LIST_DIR}/../iso_seams/include)

    # Enforce C11 (stdatomic). The standard is set by the target property, not by -std=...: the parent project
    # sets CMAKE_C_STANDARD=99 and CMake appends its own -std=c99 after target_compile_options, which would override a hand-written -std=c11.
    set_target_properties(iso_extent PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    # ASan, UBSan and warnings. A static library has no link step: the sanitizer link options are INTERFACE so that
    # the executables that use iso_extent get the sanitizer runtime too.
    target_compile_options(iso_extent PRIVATE
        -Wall -Wextra -Werror
        -fsanitize=address -fsanitize=undefined
    )
    target_link_options(iso_extent INTERFACE
        -fsanitize=address -fsanitize=undefined
    )
endif()
