if (NOT COMMAND idf_component_register)
    # Host tests build only the pure-C logic (see test/host/CMakeLists.txt).
    add_library(usb_hid_logic STATIC usb_hid_logic.c)
    target_include_directories(usb_hid_logic PUBLIC include)
    target_link_libraries(usb_hid_logic PUBLIC hid_logic)
else()
    idf_component_register(
        SRCS "usb_hid_logic.c" "usb_hid.c"
        INCLUDE_DIRS "include"
        REQUIRES hid_logic
        PRIV_REQUIRES esp_tinyusb esp_hw_support soc log
    )
endif()
