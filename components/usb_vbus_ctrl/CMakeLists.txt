if(COMMAND idf_component_register)
    # ESP-IDF build
    idf_component_register(SRCS "usb_vbus_ctrl.c"
                           INCLUDE_DIRS "include")
else()
    # Host build (CMake direct)
    add_library(usb_vbus_ctrl usb_vbus_ctrl.c)
    target_include_directories(usb_vbus_ctrl PUBLIC include)

    # Enforce C99, ASan, UBSan, and warnings
    target_compile_options(usb_vbus_ctrl PRIVATE
        -std=c99
        -Wall -Wextra -Werror
        -fsanitize=address -fsanitize=undefined
    )
    target_link_options(usb_vbus_ctrl PRIVATE
        -fsanitize=address -fsanitize=undefined
    )
endif()
