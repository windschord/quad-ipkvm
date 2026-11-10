if(COMMAND idf_component_register)
    # ESP-IDF build（純 C。IDF のヘッダーは使わない）
    idf_component_register(SRCS "kvm_api.c"
                           INCLUDE_DIRS "include"
                           REQUIRES kvm_ctrl)
else()
    # Host build (CMake direct)
    add_library(kvm_api kvm_api.c)
    target_include_directories(kvm_api PUBLIC include)
    target_link_libraries(kvm_api PUBLIC kvm_ctrl)

    # Enforce C99, ASan, UBSan, and warnings
    target_compile_options(kvm_api PRIVATE
        -std=c99
        -Wall -Wextra -Werror
        -fsanitize=address -fsanitize=undefined
    )
    target_link_options(kvm_api PRIVATE
        -fsanitize=address -fsanitize=undefined
    )
endif()
