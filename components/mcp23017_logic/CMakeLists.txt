if (NOT COMMAND idf_component_register)
    # We are in host test environment
    add_library(mcp23017_logic STATIC "mcp23017_logic.c")
    target_include_directories(mcp23017_logic PUBLIC "include")
else()
    # We are in ESP-IDF build
    idf_component_register(
        SRCS "mcp23017_logic.c"
        INCLUDE_DIRS "include"
    )
endif()
