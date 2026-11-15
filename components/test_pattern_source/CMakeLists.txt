idf_component_register(SRCS "test_pattern_source.c"
                       INCLUDE_DIRS "include"
                       REQUIRES frame_hub esp_timer video_status kvm_logic
                       PRIV_REQUIRES freertos)
