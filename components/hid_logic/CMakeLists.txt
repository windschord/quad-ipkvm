if (NOT COMMAND idf_component_register)
    add_library(hid_logic STATIC hid_logic.c hid_msg.c)
    target_include_directories(hid_logic PUBLIC include)
else()
    idf_component_register(SRCS "hid_logic.c" "hid_msg.c"
                           INCLUDE_DIRS "include"
                           REQUIRES "")
endif()
