if (NOT COMMAND idf_component_register)
    # We are in host test environment
    add_library(ws_clients STATIC "ws_clients.c")
    target_include_directories(ws_clients PUBLIC "include")
else()
    # We are in ESP-IDF build
    idf_component_register(
        SRCS "ws_clients.c"
        INCLUDE_DIRS "include"
    )
endif()
