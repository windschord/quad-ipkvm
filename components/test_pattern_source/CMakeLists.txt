idf_component_register(SRCS "test_pattern_source.c"
                       INCLUDE_DIRS "include"
                       REQUIRES frame_hub esp_timer
                       PRIV_REQUIRES freertos)
