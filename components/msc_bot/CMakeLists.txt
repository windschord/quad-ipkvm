# msc_bot: USB Mass Storage の Bulk-Only Transport の状態機械（純 C。P8 の C-13、docs/MSC_BOT_TESTS.md）
if(COMMAND idf_component_register)
    # ESP-IDF build
    idf_component_register(SRCS "msc_bot.c"
                           INCLUDE_DIRS "include")
else()
    # Host build (CMake direct)
    add_library(msc_bot msc_bot.c)
    target_include_directories(msc_bot PUBLIC include)

    # Enforce C99, ASan, UBSan, and warnings
    target_compile_options(msc_bot PRIVATE
        -std=c99
        -Wall -Wextra -Werror
        -fsanitize=address -fsanitize=undefined
        -fno-sanitize-recover=undefined
    )
    target_link_options(msc_bot PRIVATE
        -fsanitize=address -fsanitize=undefined
    )
endif()
