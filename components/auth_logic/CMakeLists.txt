if (NOT COMMAND idf_component_register)
    # We are in host test environment
    add_library(auth_logic STATIC "src/auth_logic.c")
    target_include_directories(auth_logic PUBLIC "include")
else()
    # We are in ESP-IDF build
    idf_component_register(
        SRCS "src/auth_logic.c"
        INCLUDE_DIRS "include"
    )
endif()
