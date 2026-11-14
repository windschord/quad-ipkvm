# IDF 専用の接着（FreeRTOS のタスクとミューテックス）。ホストのテストはない（ロジックは kvm_ctrl / kvm_api でテスト済み）。
if(COMMAND idf_component_register)
    idf_component_register(
        SRCS "kvm_service.c"
        INCLUDE_DIRS "include"
        REQUIRES kvm_api kvm_ctrl kvm_board kvm_logic freertos esp_timer log
    )
endif()
