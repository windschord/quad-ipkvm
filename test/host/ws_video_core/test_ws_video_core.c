#include "unity.h"
#include "ws_video_core.h"
#include <string.h>
#include <stdlib.h>

static ws_video_core_t core;

static int hub_reg_count = 0;
static int hub_unreg_count = 0;
static int hub_unreg_last_id = -1;
static int hub_get_count = 0;
static int hub_rel_count = 0;
static bool hub_fail_reg = false;
static int hub_next_id = 100;

static video_frame_t mock_frame = {
    .data = (uint8_t*)"test",
    .size = 4,
    .width = 1920,
    .height = 1080,
    .timestamp_ms = 1000,
    .seq = 1
};
static bool hub_return_null = false;
static bool hub_frame_released = false; // To check if release was called on the frame

static int io_begin_count = 0;
static int io_begin_ret = 0;
static int io_close_count = 0;
static ws_video_conn_t io_close_last_conn = NULL;
static bool io_val_ret = true;
static int io_val_count = 0;

static int mock_hub_register(void* user_data) {
    (void)user_data;
    if (hub_fail_reg) return -1;
    hub_reg_count++;
    return hub_next_id++;
}

static void mock_hub_unregister(void* user_data, int hub_client_id) {
    (void)user_data;
    hub_unreg_count++;
    hub_unreg_last_id = hub_client_id;
}

static const video_frame_t* mock_hub_get_latest(void* user_data, int hub_client_id) {
    (void)user_data;
    (void)hub_client_id;
    hub_get_count++;
    if (hub_return_null) return NULL;
    return &mock_frame;
}

static void mock_hub_release(void* user_data, int hub_client_id, const video_frame_t* frame) {
    (void)user_data;
    (void)hub_client_id;
    (void)frame;
    hub_rel_count++;
    hub_frame_released = true;
}

static int mock_io_begin_send(void* user_data, ws_video_conn_t conn, const uint8_t* data, size_t len) {
    (void)user_data;
    (void)conn;
    (void)data;
    (void)len;
    io_begin_count++;
    return io_begin_ret;
}

static void mock_io_request_close(void* user_data, ws_video_conn_t conn) {
    (void)user_data;
    io_close_count++;
    io_close_last_conn = conn;
}

static bool mock_io_validate_session(void* user_data, const uint8_t token[32]) {
    (void)user_data;
    (void)token;
    io_val_count++;
    return io_val_ret;
}

static void reset_mocks(void) {
    hub_reg_count = 0;
    hub_unreg_count = 0;
    hub_unreg_last_id = -1;
    hub_get_count = 0;
    hub_rel_count = 0;
    hub_fail_reg = false;
    hub_next_id = 100;
    hub_return_null = false;
    hub_frame_released = false;

    io_begin_count = 0;
    io_begin_ret = 0;
    io_close_count = 0;
    io_close_last_conn = NULL;
    io_val_ret = true;
    io_val_count = 0;

    mock_frame.seq = 1;
}

void setUp(void) {
    reset_mocks();
    ws_video_core_config_t cfg = {
        .max_clients = 2,
        .session_check_interval_ms = 1000,
        .hub_register = mock_hub_register,
        .hub_unregister = mock_hub_unregister,
        .hub_get_latest = mock_hub_get_latest,
        .hub_release = mock_hub_release,
        .io_begin_send = mock_io_begin_send,
        .io_request_close = mock_io_request_close,
        .io_validate_session = mock_io_validate_session,
        .user_data = NULL
    };
    TEST_ASSERT_EQUAL(0, ws_video_core_init(&core, &cfg));
}

void tearDown(void) {
    ws_video_core_destroy(&core);
}

void test_basic_connect_close(void) {
    uint8_t token[32] = {1};
    int slot = ws_video_core_on_connect(&core, (ws_video_conn_t)1, 10, token, 0);
    TEST_ASSERT_EQUAL(0, slot);
    TEST_ASSERT_EQUAL(1, hub_reg_count);

    ws_video_core_on_close(&core, 10);
    TEST_ASSERT_EQUAL(1, hub_unreg_count);
    TEST_ASSERT_EQUAL(100, hub_unreg_last_id); // The ID assigned

    // Check token zeroing
    for (int i=0; i<32; i++) {
        TEST_ASSERT_EQUAL(0, core.tokens[0][i]);
    }
}

void test_defer_unregister_during_send(void) {
    // 1. Defer unregister during send.
    uint8_t token[32] = {1};
    ws_video_core_on_connect(&core, (ws_video_conn_t)1, 10, token, 0);

    ws_video_core_tick(&core, 0);
    TEST_ASSERT_EQUAL(1, io_begin_count);

    ws_video_core_on_close(&core, 10);
    // Unregister should NOT be called yet
    TEST_ASSERT_EQUAL(0, hub_unreg_count);

    ws_video_core_on_send_done(&core, 10, true);
    // Unregister should be called now
    TEST_ASSERT_EQUAL(1, hub_unreg_count);
    TEST_ASSERT_EQUAL(1, hub_rel_count); // Released frame on send done
}

void test_send_failure_releases_and_closes(void) {
    // on_send_done(ok=false) releases frame and requests close.
    uint8_t token[32] = {1};
    ws_video_core_on_connect(&core, (ws_video_conn_t)1, 10, token, 0);

    ws_video_core_tick(&core, 0);

    ws_video_core_on_send_done(&core, 10, false);
    TEST_ASSERT_EQUAL(1, hub_rel_count);
    TEST_ASSERT_EQUAL(1, io_close_count);
    TEST_ASSERT_EQUAL((ws_video_conn_t)1, io_close_last_conn);
}

void test_begin_send_failure_releases_and_closes(void) {
    // begin_send failure releases frame and updates state.
    uint8_t token[32] = {1};
    ws_video_core_on_connect(&core, (ws_video_conn_t)1, 10, token, 0);

    io_begin_ret = -1; // Fail begin_send

    ws_video_core_tick(&core, 0);

    TEST_ASSERT_EQUAL(1, io_begin_count);
    TEST_ASSERT_EQUAL(1, hub_rel_count);
    TEST_ASSERT_EQUAL(1, io_close_count);

    // Check that last_seq is NOT updated
    TEST_ASSERT_EQUAL(-1, core.last_seqs[0]);
}

void test_duplicate_seq_not_sent(void) {
    // 6. No duplicate seq sent
    uint8_t token[32] = {1};
    ws_video_core_on_connect(&core, (ws_video_conn_t)1, 10, token, 0);

    ws_video_core_tick(&core, 0);
    TEST_ASSERT_EQUAL(1, io_begin_count);

    ws_video_core_on_send_done(&core, 10, true);
    TEST_ASSERT_EQUAL(1, hub_rel_count);

    // Try send again, same seq
    hub_frame_released = false;
    ws_video_core_tick(&core, 0);
    // io_begin should NOT increase
    TEST_ASSERT_EQUAL(1, io_begin_count);
    // It should fetch and release it immediately
    TEST_ASSERT_EQUAL(2, hub_get_count);
    TEST_ASSERT_EQUAL(2, hub_rel_count);
    TEST_ASSERT_TRUE(hub_frame_released);
}

void test_on_client_data_closes(void) {
    // 7. on_client_data triggers close
    uint8_t token[32] = {1};
    ws_video_core_on_connect(&core, (ws_video_conn_t)1, 10, token, 0);

    ws_video_core_on_client_data(&core, 10);
    TEST_ASSERT_EQUAL(1, io_close_count);
}

void test_session_timeout(void) {
    // 5. Session timeout triggers close
    uint8_t token[32] = {1};
    ws_video_core_on_connect(&core, (ws_video_conn_t)1, 10, token, 0);

    // 1000ms later, should check session
    io_val_ret = false;
    ws_video_core_tick(&core, 1000);

    TEST_ASSERT_EQUAL(1, io_val_count);
    TEST_ASSERT_EQUAL(1, io_close_count);
}

void test_hub_fail_full_dup(void) {
    // 8. Full/Duplicate/Hub-Fail handled
    uint8_t token[32] = {1};

    // Hub fail
    hub_fail_reg = true;
    int res = ws_video_core_on_connect(&core, (ws_video_conn_t)1, 10, token, 0);
    TEST_ASSERT_EQUAL(-3, res);
    hub_fail_reg = false;

    // Success 1
    res = ws_video_core_on_connect(&core, (ws_video_conn_t)1, 10, token, 0);
    TEST_ASSERT_EQUAL(0, res);

    // Duplicate
    res = ws_video_core_on_connect(&core, (ws_video_conn_t)2, 10, token, 0);
    TEST_ASSERT_EQUAL(-2, res);

    // Success 2
    res = ws_video_core_on_connect(&core, (ws_video_conn_t)2, 11, token, 0);
    TEST_ASSERT_EQUAL(1, res);

    // Full
    res = ws_video_core_on_connect(&core, (ws_video_conn_t)3, 12, token, 0);
    TEST_ASSERT_EQUAL(-1, res);
}

void test_destroy_zeros_memory(void) {
    uint8_t token[32] = {0xAA};
    ws_video_core_on_connect(&core, (ws_video_conn_t)1, 10, token, 0);

    ws_video_core_destroy(&core);

    // Ensure memory is zeroed out
    TEST_ASSERT_EQUAL(0, core.tokens[0][0]);
    TEST_ASSERT_EQUAL(0, core.config.max_clients);
}


/* Invariant Automated Test (Fuzzer-like) */

// Mock state for invariants
static int f_active_conns = 0;
static int f_held_frames[10] = {0}; // hub_client_id -> ref_count
static bool f_fail_asserts = false;

static int f_mock_hub_register(void* user_data) {
    (void)user_data;
    if (f_active_conns >= 10) return -1; // arbitrary test limit
    int id = f_active_conns++;
    f_held_frames[id] = 0;
    return id;
}

static void f_mock_hub_unregister(void* user_data, int hub_client_id) {
    (void)user_data;
    if (hub_client_id < 0 || hub_client_id >= f_active_conns) {
        if (!f_fail_asserts) TEST_FAIL_MESSAGE("Unregistered invalid ID");
    }
    if (f_held_frames[hub_client_id] > 0) {
        // frame_hub_unregister_client actually drops the held reference.
        // The prompt says: "unregister_client が保持中のフレームの参照を落とすことを確認する".
        f_held_frames[hub_client_id] = 0;
    }
}

static const video_frame_t* f_mock_hub_get_latest(void* user_data, int hub_client_id) {
    (void)user_data;
    if (hub_client_id < 0 || hub_client_id >= f_active_conns) return NULL;
    if (f_held_frames[hub_client_id] > 0) {
        if (!f_fail_asserts) TEST_FAIL_MESSAGE("Got new frame while already holding one");
    }
    f_held_frames[hub_client_id]++;
    return &mock_frame;
}

static void f_mock_hub_release(void* user_data, int hub_client_id, const video_frame_t* frame) {
    (void)user_data;
    (void)frame;
    if (f_held_frames[hub_client_id] <= 0) {
        if (!f_fail_asserts) TEST_FAIL_MESSAGE("Double release or released without hold");
    }
    f_held_frames[hub_client_id]--;
}

void test_fuzz_invariants(void) {
    ws_video_core_destroy(&core);

    ws_video_core_config_t cfg = {
        .max_clients = 5,
        .session_check_interval_ms = 1000,
        .hub_register = f_mock_hub_register,
        .hub_unregister = f_mock_hub_unregister,
        .hub_get_latest = f_mock_hub_get_latest,
        .hub_release = f_mock_hub_release,
        .io_begin_send = mock_io_begin_send,
        .io_request_close = mock_io_request_close,
        .io_validate_session = mock_io_validate_session,
        .user_data = NULL
    };

    TEST_ASSERT_EQUAL(0, ws_video_core_init(&core, &cfg));

    srand(12345); // Fixed seed for reproducibility

    int fds[5] = {-1, -1, -1, -1, -1};
    uint64_t time = 0;

    for (int step = 0; step < 1000; step++) {
        int r = rand() % 100;
        int slot = rand() % 5;

        if (r < 20) { // Connect
            if (fds[slot] == -1) {
                uint8_t token[32] = {0};
                int fd = slot + 10;
                if (ws_video_core_on_connect(&core, (ws_video_conn_t)(uintptr_t)fd, fd, token, time) >= 0) {
                    fds[slot] = fd;
                }
            }
        } else if (r < 40) { // Close
            if (fds[slot] != -1) {
                ws_video_core_on_close(&core, fds[slot]);
                // We keep it in fds until free if sending, but simulate client dropping:
                // If it's sending, it stays CLOSING.
            }
        } else if (r < 60) { // Send Done
            if (fds[slot] != -1) {
                bool ok = (rand() % 2) == 0;
                ws_video_core_on_send_done(&core, fds[slot], ok);
                if (core.clients.clients[slot].state == WS_CLIENT_FREE) {
                    fds[slot] = -1;
                }
            }
        } else if (r < 80) { // Tick
            time += 100;
            mock_frame.seq = time / 500 + 1; // New frame every 500ms
            ws_video_core_tick(&core, time);
        } else { // Client data
             if (fds[slot] != -1) {
                 ws_video_core_on_client_data(&core, fds[slot]);
             }
        }
    }
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_basic_connect_close);
    RUN_TEST(test_defer_unregister_during_send);
    RUN_TEST(test_send_failure_releases_and_closes);
    RUN_TEST(test_begin_send_failure_releases_and_closes);
    RUN_TEST(test_duplicate_seq_not_sent);
    RUN_TEST(test_on_client_data_closes);
    RUN_TEST(test_session_timeout);
    RUN_TEST(test_hub_fail_full_dup);
    RUN_TEST(test_destroy_zeros_memory);
    RUN_TEST(test_fuzz_invariants);
    return UNITY_END();
}
