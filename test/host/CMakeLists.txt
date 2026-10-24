cmake_minimum_required(VERSION 3.16)
project(quad-ipkvm-host-tests C)

# Use C99
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# Enable CTest
enable_testing()

# Fetch Unity testing framework
include(FetchContent)
FetchContent_Declare(
  unity
  GIT_REPOSITORY https://github.com/ThrowTheSwitch/Unity.git
  GIT_TAG        v2.6.0
)
FetchContent_MakeAvailable(unity)

# Setup kvm_logic component
add_subdirectory(${CMAKE_SOURCE_DIR}/../../components/kvm_logic ${CMAKE_BINARY_DIR}/kvm_logic)

add_subdirectory(${CMAKE_SOURCE_DIR}/../../components/web_server ${CMAKE_BINARY_DIR}/web_server)

# Setup auth_logic component (pure C)
add_subdirectory(${CMAKE_SOURCE_DIR}/../../components/auth_logic ${CMAKE_BINARY_DIR}/auth_logic)

# Create a lightweight static library for testing pure C logic from tc358743
add_library(tc358743_edid STATIC ${CMAKE_SOURCE_DIR}/../../components/tc358743/tc358743_edid.c)
target_include_directories(tc358743_edid PUBLIC ${CMAKE_SOURCE_DIR}/../../components/tc358743/include)

# Create a static library for auth test reference crypto
add_library(auth_crypto_ref STATIC ${CMAKE_SOURCE_DIR}/auth_logic/auth_crypto_ref.c)
target_include_directories(auth_crypto_ref PUBLIC ${CMAKE_SOURCE_DIR}/../../components/auth_logic/include)


# Discover tests automatically
file(GLOB_RECURSE TEST_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/*/test_*.c" "${CMAKE_CURRENT_SOURCE_DIR}/test_*.c")
list(FILTER TEST_SOURCES EXCLUDE REGEX ".*build/.*")

# Common compiler options for tests
set(TEST_COMPILE_OPTIONS
    -Wall -Wextra -Werror
    -fsanitize=address -fsanitize=undefined
    -fno-sanitize-recover=undefined
)
set(TEST_LINK_OPTIONS
    -fsanitize=address -fsanitize=undefined
)

foreach(test_source ${TEST_SOURCES})
    get_filename_component(test_name ${test_source} NAME_WE)

    add_executable(${test_name} ${test_source})
    # Link kvm_logic for all, and conditionally link tc358743_edid for its tests
    target_link_libraries(${test_name} PRIVATE unity kvm_logic)

    if (test_source MATCHES "test_edid.c")
        target_link_libraries(${test_name} PRIVATE tc358743_edid)
    endif()

    if (test_source MATCHES "/http_guard/")
        target_link_libraries(${test_name} PRIVATE http_guard auth_logic)
    endif()

    if (test_source MATCHES "/auth_logic/")
        target_link_libraries(${test_name} PRIVATE auth_logic auth_crypto_ref)
    endif()

    if (test_source MATCHES "/frame_hub/")
        target_sources(${test_name} PRIVATE ${CMAKE_SOURCE_DIR}/../../components/frame_hub/frame_hub.c)
        target_include_directories(${test_name} PRIVATE ${CMAKE_SOURCE_DIR}/../../components/frame_hub/include)
    endif()

    if (test_source MATCHES "/web_server/")
        target_link_libraries(${test_name} PRIVATE web_status_map)
    endif()

    if (test_source MATCHES "/ws_clients/")
        target_link_libraries(${test_name} PRIVATE ws_clients)
    endif()

    if (test_source MATCHES "/web_assets/")
        target_sources(${test_name} PRIVATE ${CMAKE_SOURCE_DIR}/../../components/web_server/web_assets.c)
        target_include_directories(${test_name} PRIVATE ${CMAKE_SOURCE_DIR}/../../components/web_server/include)
    endif()

    if (FALSE)
        target_link_libraries(${test_name} PRIVATE ws_clients)
    endif()


    target_compile_options(${test_name} PRIVATE ${TEST_COMPILE_OPTIONS})
    target_link_options(${test_name} PRIVATE ${TEST_LINK_OPTIONS})

    add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()

# Setup http_guard component (pure C)
add_subdirectory(${CMAKE_SOURCE_DIR}/../../components/http_guard ${CMAKE_BINARY_DIR}/http_guard)

# Setup ws_clients component
add_subdirectory(${CMAKE_SOURCE_DIR}/../../components/ws_clients ${CMAKE_BINARY_DIR}/ws_clients)
