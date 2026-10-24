idf_component_register(SRCS "main.c" INCLUDE_DIRS "." REQUIRES tc358743 esp_driver_i2c net_eth frame_hub test_pattern_source web_server web_cert nvs_flash)
