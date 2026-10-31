if (NOT COMMAND idf_component_register)
    # For host tests
    add_library(mipi_phy_power STATIC mipi_phy_power.c)
    target_include_directories(mipi_phy_power PUBLIC .)
else()
    # For ESP-IDF
    idf_component_register(
        SRCS "mipi_phy_power.c"
        INCLUDE_DIRS "."
        REQUIRES esp_hw_support
    )
endif()
