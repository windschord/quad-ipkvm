if(NOT COMMAND idf_component_register)
    add_library(ws_input_core STATIC "ws_input_core.c")
    target_include_directories(ws_input_core PUBLIC "include" "../hid_logic/include" "../ws_clients/include")
    target_link_libraries(ws_input_core PUBLIC hid_logic ws_clients)
else()
    idf_component_register(
        SRCS "ws_input_core.c"
        INCLUDE_DIRS "include"
        REQUIRES hid_logic ws_clients
    )
endif()
