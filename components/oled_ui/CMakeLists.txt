if (NOT COMMAND idf_component_register)
    # We are in host test environment
    add_library(oled_ui STATIC "oled_ui.c")
    target_include_directories(oled_ui PUBLIC "include")
else()
    # We are in ESP-IDF build
    idf_component_register(
        SRCS "oled_ui.c"
        INCLUDE_DIRS "include"
    )
endif()
