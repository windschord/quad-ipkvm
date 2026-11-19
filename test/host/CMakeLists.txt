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
add_subdirectory(../../components/mipi_phy_power mipi_phy_power)
add_subdirectory(${CMAKE_SOURCE_DIR}/../../components/kvm_logic ${CMAKE_BINARY_DIR}/kvm_logic)

add_subdirectory(${CMAKE_SOURCE_DIR}/../../components/web_server ${CMAKE_BINARY_DIR}/web_server)

# Setup auth_logic component (pure C)
add_subdirectory(${CMAKE_SOURCE_DIR}/../../components/auth_logic ${CMAKE_BINARY_DIR}/auth_logic)

# Create a lightweight static library for testing pure C logic from tc358743
add_library(tc358743_edid STATIC ${CMAKE_SOURCE_DIR}/../../components/tc358743/tc358743_edid.c)
target_include_directories(tc358743_edid PUBLIC ${CMAKE_SOURCE_DIR}/../../components/tc358743/include)

# Create a static library for pll calc test
add_library(tc358743_pll_calc STATIC ${CMAKE_SOURCE_DIR}/../../components/tc358743/tc358743_pll_calc.c)
target_include_directories(tc358743_pll_calc PUBLIC ${CMAKE_SOURCE_DIR}/../../components/tc358743/include)

# Create a static library for auth test reference crypto
add_library(auth_crypto_ref STATIC ${CMAKE_SOURCE_DIR}/auth_logic/auth_crypto_ref.c)
target_include_directories(auth_crypto_ref PUBLIC ${CMAKE_SOURCE_DIR}/../../components/auth_logic/include)



# Create a static library for tc358743_start test
add_library(tc358743_start STATIC ${CMAKE_SOURCE_DIR}/../../components/tc358743/tc358743_start.c)
target_include_directories(tc358743_start PUBLIC ${CMAKE_SOURCE_DIR}/../../components/tc358743/include)
target_link_libraries(tc358743_start PRIVATE tc358743_edid tc358743_pll_calc)

# Pure-C part of csi_capture (geometry check and two-buffer selector)
add_library(csi_capture_logic STATIC ${CMAKE_SOURCE_DIR}/../../components/csi_capture/csi_capture_logic.c)
target_include_directories(csi_capture_logic PUBLIC ${CMAKE_SOURCE_DIR}/../../components/csi_capture/include)

# Pure-C part of csi_video_source (JPEG validation, output sizing, seqlock status, publish stats)
add_subdirectory(${CMAKE_SOURCE_DIR}/../../components/csi_video_source ${CMAKE_BINARY_DIR}/csi_video_source)

# Pure-C part of csi_h264 (H.264 geometry, Annex B frame validation, codec/IDR state machine)
add_subdirectory(${CMAKE_SOURCE_DIR}/../../components/csi_h264 ${CMAKE_BINARY_DIR}/csi_h264)

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
    if (test_source MATCHES "/net_eth/")
        target_link_libraries(${test_name} PRIVATE net_eth_util)
    endif()

    if (test_source MATCHES "/mipi_phy_power/")
        target_link_libraries(${test_name} PRIVATE mipi_phy_power)
    endif()

    if (test_source MATCHES "test_edid.c")
        target_link_libraries(${test_name} PRIVATE tc358743_edid)
    endif()

    if (test_source MATCHES "test_pll_calc.c")
        target_link_libraries(${test_name} PRIVATE tc358743_pll_calc)
    endif()


    if (test_source MATCHES "test_start_csi.c")
        target_link_libraries(${test_name} PRIVATE tc358743_start tc358743_edid)
    endif()

    if (test_source MATCHES "/csi_capture/")
        target_link_libraries(${test_name} PRIVATE csi_capture_logic)
    endif()

    if (test_source MATCHES "/csi_video_source/")
        target_link_libraries(${test_name} PRIVATE csi_video_source_logic)
    endif()

    if (test_source MATCHES "/csi_h264/")
        target_link_libraries(${test_name} PRIVATE csi_h264_logic)
    endif()



    if (test_source MATCHES "/http_guard/")
        target_link_libraries(${test_name} PRIVATE http_guard auth_logic)
    endif()

    if (test_source MATCHES "/hid_logic/")
        target_link_libraries(${test_name} PRIVATE hid_logic)
    endif()

    if (test_source MATCHES "/auth_worker_slot/")
        target_link_libraries(${test_name} PRIVATE auth_worker_slot)
        target_include_directories(${test_name} PRIVATE ${CMAKE_SOURCE_DIR}/../../components/auth_worker_slot/include)
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

    if (test_source MATCHES "/ws_video_core/")
        target_link_libraries(${test_name} PRIVATE ws_video_core)
    endif()

    if (test_source MATCHES "/ws_input_core/")
        target_link_libraries(${test_name} PRIVATE ws_input_core)
    endif()

    if (test_source MATCHES "/oled_ui/")
        target_link_libraries(${test_name} PRIVATE oled_ui)
        target_include_directories(${test_name} PRIVATE ${CMAKE_SOURCE_DIR}/../../components/oled_ui/include)
    endif()

    if (test_source MATCHES "/web_assets/")
        target_sources(${test_name} PRIVATE ${CMAKE_SOURCE_DIR}/../../components/web_server/web_assets.c ${CMAKE_SOURCE_DIR}/../../components/web_server/web_headers.c)
        target_include_directories(${test_name} PRIVATE ${CMAKE_SOURCE_DIR}/../../components/web_server/include)
        target_compile_definitions(${test_name} PRIVATE WEB_ASSET_DIR="${CMAKE_SOURCE_DIR}/../../components/web_server/web")
    endif()

    if (test_source MATCHES "/oled_render/")
        target_link_libraries(${test_name} PRIVATE oled_render)
    endif()

    if (test_source MATCHES "/mcp23017_logic/")
        target_link_libraries(${test_name} PRIVATE mcp23017_logic)
    endif()

    if (test_source MATCHES "/usb_hid/")
        target_link_libraries(${test_name} PRIVATE usb_hid_logic)
    endif()

    if (test_source MATCHES "/kvm_board/")
        target_link_libraries(${test_name} PRIVATE kvm_board_mock)
    endif()

    if (test_source MATCHES "/kvm_ctrl/")
        target_link_libraries(${test_name} PRIVATE kvm_ctrl kvm_board_mock)
    endif()

    if (test_source MATCHES "/video_status/")
        target_link_libraries(${test_name} PRIVATE video_status kvm_ctrl kvm_board_mock)
    endif()

    if (test_source MATCHES "/video_codec_policy/")
        target_link_libraries(${test_name} PRIVATE video_codec_policy)
    endif()

    if (test_source MATCHES "/usb_vbus_ctrl/")
        target_link_libraries(${test_name} PRIVATE usb_vbus_ctrl)
    endif()

    if (test_source MATCHES "/iso_media/")
        target_link_libraries(${test_name} PRIVATE iso_media)
    endif()

    if (test_source MATCHES "/mmc_scsi/")
        target_link_libraries(${test_name} PRIVATE mmc_scsi iso_media)
        target_include_directories(${test_name} PRIVATE ${CMAKE_SOURCE_DIR}/../../components/iso_seams/include)
    endif()

    if (test_source MATCHES "/kvm_api/")
        target_link_libraries(${test_name} PRIVATE kvm_api kvm_ctrl kvm_board_mock http_guard auth_logic)
    endif()

    # ホストテストのディレクトリからの相対パスで選ぶ（チェックアウト先の絶対パスに /msc_bot/ や /iso_catalog/ が含まれても、他のテストにはリンクしない）
    file(RELATIVE_PATH test_rel_path ${CMAKE_CURRENT_SOURCE_DIR} ${test_source})

    if (test_rel_path MATCHES "^iso_extent/")
        target_link_libraries(${test_name} PRIVATE iso_extent pthread)
    endif()

    if (test_rel_path MATCHES "^iso_catalog/")
        target_link_libraries(${test_name} PRIVATE iso_catalog iso_mock_fs)
        target_include_directories(${test_name} PRIVATE ${CMAKE_SOURCE_DIR}/iso_catalog)
    endif()

    # msc_bot のテストだけ
    if (test_rel_path MATCHES "^msc_bot/")
        target_link_libraries(${test_name} PRIVATE msc_bot)
    endif()

    if (test_rel_path MATCHES "^iso_usb_msc/")
        target_link_libraries(${test_name} PRIVATE iso_usb_msc_glue pthread)
        set_target_properties(${test_name} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    endif()



    target_compile_options(${test_name} PRIVATE ${TEST_COMPILE_OPTIONS})
    target_link_options(${test_name} PRIVATE ${TEST_LINK_OPTIONS})

    add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()

# Setup http_guard component (pure C)
add_subdirectory(${CMAKE_SOURCE_DIR}/../../components/http_guard ${CMAKE_BINARY_DIR}/http_guard)

# Setup ws_clients component
add_subdirectory(${CMAKE_SOURCE_DIR}/../../components/ws_clients ${CMAKE_BINARY_DIR}/ws_clients)

# Create a lightweight static library for frame_hub for host tests
add_library(frame_hub STATIC ${CMAKE_SOURCE_DIR}/../../components/frame_hub/frame_hub.c)
target_include_directories(frame_hub PUBLIC ${CMAKE_SOURCE_DIR}/../../components/frame_hub/include)

# Setup ws_video_core component
add_subdirectory(${CMAKE_SOURCE_DIR}/../../components/ws_video_core ${CMAKE_BINARY_DIR}/ws_video_core)

# Setup ws_input_core component
add_subdirectory(${CMAKE_SOURCE_DIR}/../../components/ws_input_core ${CMAKE_BINARY_DIR}/ws_input_core)

# Setup oled_render component
add_subdirectory(${CMAKE_SOURCE_DIR}/../../components/oled_render ${CMAKE_BINARY_DIR}/oled_render)

# Setup mcp23017_logic component
add_subdirectory(${CMAKE_SOURCE_DIR}/../../components/mcp23017_logic ${CMAKE_BINARY_DIR}/mcp23017_logic)

# Setup hid_logic component (pure C)
add_subdirectory(${CMAKE_SOURCE_DIR}/../../components/hid_logic ${CMAKE_BINARY_DIR}/hid_logic)

# Setup oled_ui component (pure C)
add_subdirectory(${CMAKE_SOURCE_DIR}/../../components/oled_ui ${CMAKE_BINARY_DIR}/oled_ui)

# Setup auth_worker_slot component (pure C)
add_subdirectory(${CMAKE_SOURCE_DIR}/../../components/auth_worker_slot ${CMAKE_BINARY_DIR}/auth_worker_slot)

# Setup net_eth_util component (pure C)
add_subdirectory(${CMAKE_SOURCE_DIR}/../../components/net_eth ${CMAKE_BINARY_DIR}/net_eth)

# Setup usb_hid_logic component (pure C)
add_subdirectory(${CMAKE_SOURCE_DIR}/../../components/usb_hid ${CMAKE_BINARY_DIR}/usb_hid)

# Setup kvm_board (HAL header) and kvm_ctrl (kvm_logic <-> HAL glue, pure C)
add_subdirectory(${CMAKE_SOURCE_DIR}/../../components/kvm_board ${CMAKE_BINARY_DIR}/kvm_board)
add_subdirectory(${CMAKE_SOURCE_DIR}/../../components/kvm_ctrl ${CMAKE_BINARY_DIR}/kvm_ctrl)

# video_status (provider of the video sync state: video sources register, KVM reads; pure C)
add_subdirectory(${CMAKE_SOURCE_DIR}/../../components/video_status ${CMAKE_BINARY_DIR}/video_status)

# video_codec_policy (stateful codec decision MJPEG / H.264, docs/VIDEO_TRANSPORT.md 9.5; pure C)
add_subdirectory(${CMAKE_SOURCE_DIR}/../../components/video_codec_policy ${CMAKE_BINARY_DIR}/video_codec_policy)

# usb_vbus_ctrl (VBUS sensing -> D+ pull-up connect/disconnect decision, docs/USB_VBUS_DESIGN.md; pure C)
add_subdirectory(${CMAKE_SOURCE_DIR}/../../components/usb_vbus_ctrl ${CMAKE_BINARY_DIR}/usb_vbus_ctrl)

# kvm_api (HTTP API of KVM/ATX: strict body parser, JSON, result -> HTTP mapping, pure C)
add_subdirectory(${CMAKE_SOURCE_DIR}/../../components/kvm_api ${CMAKE_BINARY_DIR}/kvm_api)

# iso_extent (P8: CD block -> SD sector spans, extent table with refcount; pure C)
add_subdirectory(${CMAKE_SOURCE_DIR}/../../components/iso_extent ${CMAKE_BINARY_DIR}/iso_extent)

# iso_catalog (P8: meta codec, 2 slots, name rule, scan, import, delete steps; pure C)
add_subdirectory(${CMAKE_SOURCE_DIR}/../../components/iso_catalog ${CMAKE_BINARY_DIR}/iso_catalog)

# In-memory iso_fs_t with power-cut injection (for the iso_catalog host tests)
add_library(iso_mock_fs STATIC ${CMAKE_SOURCE_DIR}/iso_catalog/iso_mock_fs.c)
target_include_directories(iso_mock_fs PUBLIC ${CMAKE_SOURCE_DIR}/iso_catalog ${CMAKE_SOURCE_DIR}/../../components/iso_seams/include)
target_compile_options(iso_mock_fs PRIVATE ${TEST_COMPILE_OPTIONS})
target_link_options(iso_mock_fs PRIVATE ${TEST_LINK_OPTIONS})

# iso_media (virtual CD-ROM media state: EMPTY/LOADED, unit attention, prevent removal; docs/ISO_MOUNT_DESIGN.md 5.5; pure C)
add_subdirectory(${CMAKE_SOURCE_DIR}/../../components/iso_media ${CMAKE_BINARY_DIR}/iso_media)

# mmc_scsi (CD-ROM SCSI/MMC commands for the virtual drive; docs/ISO_MOUNT_DESIGN.md 5.4; pure C)
add_subdirectory(${CMAKE_SOURCE_DIR}/../../components/mmc_scsi ${CMAKE_BINARY_DIR}/mmc_scsi)

# Mock kvm_board_hal_t (reproduces the board-side obligations of docs/INTERFACES.md 6.2)
add_library(kvm_board_mock STATIC ${CMAKE_SOURCE_DIR}/kvm_board/kvm_board_mock.c)
target_include_directories(kvm_board_mock PUBLIC ${CMAKE_SOURCE_DIR}/kvm_board)
target_link_libraries(kvm_board_mock PUBLIC kvm_board)
target_compile_options(kvm_board_mock PRIVATE ${TEST_COMPILE_OPTIONS})
target_link_options(kvm_board_mock PRIVATE ${TEST_LINK_OPTIONS})

# Browser-side code (components/web_server/web/app.js) checked with node (no dependencies). Skipped when node is absent.
find_program(NODE_EXECUTABLE node)
if(NODE_EXECUTABLE)
    add_test(NAME check_video_viewer COMMAND ${NODE_EXECUTABLE} ${CMAKE_SOURCE_DIR}/../../tools/test/check_video_viewer.js)
else()
    message(STATUS "node not found: check_video_viewer is not registered")
endif()

# msc_bot (USB Mass Storage Bulk-Only Transport state machine, docs/MSC_BOT_TESTS.md; pure C)
add_subdirectory(${CMAKE_SOURCE_DIR}/../../components/msc_bot ${CMAKE_BINARY_DIR}/msc_bot)

# iso_usb_msc_glue (msc_bot <-> mmc_scsi <-> iso_extent bridge, pure C)
add_subdirectory(${CMAKE_SOURCE_DIR}/../../components/iso_usb_msc ${CMAKE_BINARY_DIR}/iso_usb_msc)
