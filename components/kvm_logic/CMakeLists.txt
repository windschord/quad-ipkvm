if(COMMAND idf_component_register)
    # ESP-IDF build
    idf_component_register(SRCS "kvm_logic.c"
                        INCLUDE_DIRS "include")
else()
    # Host build (CMake direct)
    add_library(kvm_logic kvm_logic.c)
    target_include_directories(kvm_logic PUBLIC include)

    # Enforce C99, ASan, UBSan, and warnings
    target_compile_options(kvm_logic PRIVATE
        -std=c99
        -Wall -Wextra -Werror
        -fsanitize=address -fsanitize=undefined
    )
    target_link_options(kvm_logic PRIVATE
        -fsanitize=address -fsanitize=undefined
    )
endif()
