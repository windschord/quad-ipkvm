if (NOT COMMAND idf_component_register)
    add_library(http_guard STATIC "http_guard.c")
    target_include_directories(http_guard PUBLIC "include")
    target_link_libraries(http_guard PUBLIC auth_logic)
else()
    idf_component_register(
        SRCS "http_guard.c"
        INCLUDE_DIRS "include"
        REQUIRES auth_logic
    )
endif()
