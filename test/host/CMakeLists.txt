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

# Discover tests automatically
file(GLOB_RECURSE TEST_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/*/test_*.c" "${CMAKE_CURRENT_SOURCE_DIR}/test_*.c")
list(FILTER TEST_SOURCES EXCLUDE REGEX ".*build/.*")

# Common compiler options for tests
set(TEST_COMPILE_OPTIONS
    -Wall -Wextra -Werror
    -fsanitize=address -fsanitize=undefined
)
set(TEST_LINK_OPTIONS
    -fsanitize=address -fsanitize=undefined
)

foreach(test_source ${TEST_SOURCES})
    get_filename_component(test_name ${test_source} NAME_WE)

    add_executable(${test_name} ${test_source})
    target_link_libraries(${test_name} PRIVATE unity kvm_logic)
    target_compile_options(${test_name} PRIVATE ${TEST_COMPILE_OPTIONS})
    target_link_options(${test_name} PRIVATE ${TEST_LINK_OPTIONS})

    add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()
