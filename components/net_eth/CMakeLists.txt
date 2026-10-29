if(NOT COMMAND idf_component_register)
    add_library(net_eth_util STATIC "net_eth_util.c")
    target_include_directories(net_eth_util PUBLIC "include")
else()
    idf_component_register(
        SRCS "net_eth.c" "net_eth_util.c"
        INCLUDE_DIRS "include"
        REQUIRES esp_eth esp_netif esp_event log
    )
endif()
