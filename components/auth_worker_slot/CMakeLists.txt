if (NOT COMMAND idf_component_register)
    add_library(auth_worker_slot STATIC src/auth_worker_slot.c)
    target_include_directories(auth_worker_slot PUBLIC include)
else()
    idf_component_register(
        SRCS "src/auth_worker_slot.c"
        INCLUDE_DIRS "include"
    )
endif()
