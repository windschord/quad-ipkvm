if(COMMAND idf_component_register)
    # ESP-IDF build
    idf_component_register(SRCS "kvm_ctrl.c"
                           INCLUDE_DIRS "include"
                           REQUIRES kvm_logic kvm_board)
else()
    # Host build (CMake direct)
    add_library(kvm_ctrl kvm_ctrl.c)
    target_include_directories(kvm_ctrl PUBLIC include)
    target_link_libraries(kvm_ctrl PUBLIC kvm_logic kvm_board)

    # Enforce C99, ASan, UBSan, and warnings
    target_compile_options(kvm_ctrl PRIVATE
        -std=c99
        -Wall -Wextra -Werror
        -fsanitize=address -fsanitize=undefined
    )
    target_link_options(kvm_ctrl PRIVATE
        -fsanitize=address -fsanitize=undefined
    )
endif()
