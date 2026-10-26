if (NOT COMMAND idf_component_register)
    # We are in host test environment
    add_library(oled_render STATIC "oled_render.c" "font5x7.c")
    target_include_directories(oled_render PUBLIC "include")
else()
    # We are in ESP-IDF build
    idf_component_register(
        SRCS "oled_render.c" "font5x7.c"
        INCLUDE_DIRS "include"
    )
endif()
