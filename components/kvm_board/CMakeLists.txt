if (NOT COMMAND idf_component_register)
    # ホスト側: kvm_board.h (インターフェース定義) + 純 C 安全ロジック kvm_board_logic
    add_library(kvm_board STATIC kvm_board_logic.c)
    target_include_directories(kvm_board PUBLIC include)
    target_link_libraries(kvm_board PUBLIC kvm_logic)
else()
    idf_component_register(
        SRCS "kvm_board_logic.c" "kvm_board.c"
        INCLUDE_DIRS "include"
        REQUIRES kvm_logic
        PRIV_REQUIRES esp_driver_gpio esp_driver_gptimer freertos hal log soc
    )
endif()
