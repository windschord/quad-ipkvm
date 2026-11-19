if(COMMAND idf_component_register)
    # ESP-IDF build
    idf_component_register(SRCS "mmc_scsi.c"
                           INCLUDE_DIRS "include"
                           REQUIRES iso_seams iso_media)
else()
    # Host build (CMake direct). iso_seams is header-only (declarations).
    add_library(mmc_scsi STATIC mmc_scsi.c)
    target_include_directories(mmc_scsi PUBLIC include ${CMAKE_CURRENT_LIST_DIR}/../iso_seams/include)
    target_link_libraries(mmc_scsi PUBLIC iso_media)

    # Enforce C99, ASan, UBSan, and warnings
    target_compile_options(mmc_scsi PRIVATE
        -std=c99
        -Wall -Wextra -Werror
        -fsanitize=address -fsanitize=undefined
    )
    target_link_options(mmc_scsi PRIVATE
        -fsanitize=address -fsanitize=undefined
    )
endif()
