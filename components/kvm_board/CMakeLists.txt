if(COMMAND idf_component_register)
    # ESP-IDF build（ヘッダーだけ。GPIO の実装は A-9）
    idf_component_register(INCLUDE_DIRS "include"
                           REQUIRES kvm_logic)
else()
    # Host build (CMake direct)
    add_library(kvm_board INTERFACE)
    target_include_directories(kvm_board INTERFACE include)
    target_link_libraries(kvm_board INTERFACE kvm_logic)
endif()
