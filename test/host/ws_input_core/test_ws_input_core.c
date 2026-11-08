#include "unity.h"
#include "ws_input_core.h"
#include <string.h>

/* Host tests of ws_input_core with the real hid_logic. The IO callbacks and the HID sink are mocks that record what
 * they get. Message bytes follow docs/WEB_API.md 6 (0x80 GRANTED, 0x81 DENIED reason, 0x82 USB_STATE state). */

#define MAX_SENDS 64
#define MAX_CLOSES 64

typedef struct {
    ws_input_conn_t conn;
    uint8_t data[4];
    size_t len;
} sent_t;

static sent_t g_sent[MAX_SENDS];
static int g_send_count;
static int g_send_result; /* what io_send returns (0 = ok, -1 = queue full) */
static ws_input_conn_t g_closed[MAX_CLOSES];
static int g_close_count;
static bool g_session_valid;
static bool g_session_valid_by_token[4]; /* token[0] indexes; used when g_per_token is set */
static bool g_per_token;
static bool g_usb_ready;
static int g_validate_calls;

static bool g_sink_ready;
static int g_kbd_count;
static hid_kbd_report_t g_kbd_last;
static int g_abs_count;
static int g_rel_count;

static int mock_send(void *u, ws_input_conn_t conn, const uint8_t *data, size_t len) {
    (void)u;
    if (g_send_count < MAX_SENDS) {
        g_sent[g_send_count].conn = conn;
        g_sent[g_send_count].len = len < 4 ? len : 4;
        memcpy(g_sent[g_send_count].data, data, g_sent[g_send_count].len);
    }
    g_send_count++;
    return g_send_result;
}
static void mock_close(void *u, ws_input_conn_t conn) {
    (void)u;
    if (g_close_count < MAX_CLOSES) g_closed[g_close_count] = conn;
    g_close_count++;
}
static bool mock_validate(void *u, const uint8_t token[32]) {
    (void)u;
    g_validate_calls++;
    if (g_per_token) return g_session_valid_by_token[token[0] & 3];
    return g_session_valid;
}
static bool mock_usb_ready(void *u) { (void)u; return g_usb_ready; }

static bool sink_ready(void *c) { (void)c; return g_sink_ready; }
static bool sink_kbd(void *c, const hid_kbd_report_t *r) { (void)c; g_kbd_count++; g_kbd_last = *r; return true; }
static bool sink_abs(void *c, const hid_abs_report_t *r) { (void)c; (void)r; g_abs_count++; return true; }
static bool sink_rel(void *c, const hid_rel_report_t *r) { (void)c; (void)r; g_rel_count++; return true; }

static hid_logic_t hid;
static ws_input_core_t core;

static ws_input_core_config_t base_config(void) {
    ws_input_core_config_t c = {
        .max_clients = 2,
        .session_check_interval_ms = 5000,
        .io_send = mock_send,
        .io_request_close = mock_close,
        .io_validate_session = mock_validate,
        .usb_ready = mock_usb_ready,
        .user_data = NULL,
    };
    return c;
}

void setUp(void) {
    memset(g_sent, 0, sizeof(g_sent));
    memset(g_closed, 0, sizeof(g_closed));
    g_send_count = 0;
    g_send_result = 0;
    g_close_count = 0;
    g_session_valid = true;
    g_per_token = false;
    memset(g_session_valid_by_token, 0, sizeof(g_session_valid_by_token));
    g_usb_ready = false;
    g_validate_calls = 0;
    g_sink_ready = true;
    g_kbd_count = 0;
    memset(&g_kbd_last, 0, sizeof(g_kbd_last));
    g_abs_count = 0;
    g_rel_count = 0;

    hid_logic_config_t hc = { .hold_timeout_ms = 3000, .rate_per_sec = 400, .rate_burst = 100 };
    hid_sink_t sink = { .ctx = NULL, .ready = sink_ready, .send_keyboard = sink_kbd, .send_abs = sink_abs, .send_rel = sink_rel };
    hid_logic_init(&hid, &hc, &sink);
    ws_input_core_config_t cc = base_config();
    TEST_ASSERT_EQUAL(0, ws_input_core_init(&core, &cc, &hid));
}
void tearDown(void) {}

static uint8_t g_token_store[4][32];
static const uint8_t *token_n(int n) {
    memset(g_token_store[n & 3], 0, 32);
    g_token_store[n & 3][0] = (uint8_t)n;
    return g_token_store[n & 3];
}
static ws_input_conn_t conn_n(int n) { return (ws_input_conn_t)(intptr_t)(1000 + n); }

static int connect_n(int n, int fd, uint64_t now) { return ws_input_core_on_connect(&core, conn_n(n), fd, token_n(n), now); }
static void key_down(int fd, uint8_t usage, uint64_t now) { uint8_t m[2] = {0x01, usage}; ws_input_core_on_frame(&core, fd, m, 2, now); }

/* ---- init ---- */
void test_init_validates_arguments(void) {
    ws_input_core_t c;
    ws_input_core_config_t cc = base_config();
    TEST_ASSERT_EQUAL(-1, ws_input_core_init(NULL, &cc, &hid));
    TEST_ASSERT_EQUAL(-1, ws_input_core_init(&c, NULL, &hid));
    TEST_ASSERT_EQUAL(-1, ws_input_core_init(&c, &cc, NULL));
    cc.max_clients = 0;
    TEST_ASSERT_EQUAL(-1, ws_input_core_init(&c, &cc, &hid));
    cc.max_clients = WS_CLIENTS_MAX + 1;
    TEST_ASSERT_EQUAL(-1, ws_input_core_init(&c, &cc, &hid));
    cc = base_config();
    cc.io_send = NULL;
    TEST_ASSERT_EQUAL(-1, ws_input_core_init(&c, &cc, &hid));
    cc = base_config();
    cc.io_request_close = NULL;
    TEST_ASSERT_EQUAL(-1, ws_input_core_init(&c, &cc, &hid));
    cc = base_config();
    cc.max_clients = 1;
    TEST_ASSERT_EQUAL(0, ws_input_core_init(&c, &cc, &hid));
    cc.max_clients = WS_CLIENTS_MAX;
    TEST_ASSERT_EQUAL(0, ws_input_core_init(&c, &cc, &hid));
}

/* ---- frame header check (before the payload is read) ---- */
void test_check_frame_boundaries(void) {
    TEST_ASSERT_TRUE(ws_input_core_check_frame(2, true, 1));
    TEST_ASSERT_TRUE(ws_input_core_check_frame(2, true, 2));
    TEST_ASSERT_TRUE(ws_input_core_check_frame(2, true, 16));
    TEST_ASSERT_FALSE(ws_input_core_check_frame(2, true, 0));
    TEST_ASSERT_FALSE(ws_input_core_check_frame(2, true, 17));
    TEST_ASSERT_FALSE(ws_input_core_check_frame(2, true, 100000));
    TEST_ASSERT_FALSE(ws_input_core_check_frame(2, false, 1)); /* fragmented */
    TEST_ASSERT_FALSE(ws_input_core_check_frame(1, true, 2));  /* text */
    TEST_ASSERT_FALSE(ws_input_core_check_frame(0, true, 2));  /* continuation */
    TEST_ASSERT_FALSE(ws_input_core_check_frame(3, true, 2));
    TEST_ASSERT_FALSE(ws_input_core_check_frame(8, true, 2));  /* close */
}

/* ---- connect ---- */
void test_first_connection_becomes_controller_and_gets_granted_then_usb_state(void) {
    g_usb_ready = true;
    TEST_ASSERT_EQUAL(0, connect_n(1, 10, 0));
    TEST_ASSERT_EQUAL(10, hid.owner_id);
    TEST_ASSERT_EQUAL(2, g_send_count);
    TEST_ASSERT_EQUAL_PTR(conn_n(1), g_sent[0].conn);
    TEST_ASSERT_EQUAL(1, g_sent[0].len);
    TEST_ASSERT_EQUAL_HEX8(0x80, g_sent[0].data[0]);
    TEST_ASSERT_EQUAL_PTR(conn_n(1), g_sent[1].conn);
    TEST_ASSERT_EQUAL(2, g_sent[1].len);
    TEST_ASSERT_EQUAL_HEX8(0x82, g_sent[1].data[0]);
    TEST_ASSERT_EQUAL_HEX8(1, g_sent[1].data[1]);
    TEST_ASSERT_EQUAL(0, g_close_count);
}

void test_usb_state_zero_when_the_target_is_not_ready(void) {
    g_usb_ready = false;
    TEST_ASSERT_EQUAL(0, connect_n(1, 10, 0));
    TEST_ASSERT_EQUAL_HEX8(0x82, g_sent[1].data[0]);
    TEST_ASSERT_EQUAL_HEX8(0, g_sent[1].data[1]);
}

void test_second_connection_is_denied_and_closed_and_the_controller_is_unchanged(void) {
    TEST_ASSERT_EQUAL(0, connect_n(1, 10, 0));
    g_send_count = 0;
    TEST_ASSERT_EQUAL(0, connect_n(2, 20, 5));
    TEST_ASSERT_EQUAL(10, hid.owner_id);
    TEST_ASSERT_EQUAL(1, g_send_count); /* only DENIED: no GRANTED, no USB_STATE */
    TEST_ASSERT_EQUAL_PTR(conn_n(2), g_sent[0].conn);
    TEST_ASSERT_EQUAL(2, g_sent[0].len);
    TEST_ASSERT_EQUAL_HEX8(0x81, g_sent[0].data[0]);
    TEST_ASSERT_EQUAL_HEX8(1, g_sent[0].data[1]);
    TEST_ASSERT_EQUAL(1, g_close_count);
    TEST_ASSERT_EQUAL_PTR(conn_n(2), g_closed[0]);
}

void test_connection_table_full_and_duplicate_fd_are_refused_without_side_effects(void) {
    TEST_ASSERT_EQUAL(0, connect_n(1, 10, 0));
    TEST_ASSERT_EQUAL(0, connect_n(2, 20, 0));
    g_send_count = 0;
    g_close_count = 0;
    TEST_ASSERT_EQUAL(-1, connect_n(3, 30, 0)); /* full (max_clients = 2) */
    TEST_ASSERT_EQUAL(-1, connect_n(1, 10, 0)); /* the same fd again */
    TEST_ASSERT_EQUAL(0, g_send_count);
    TEST_ASSERT_EQUAL(0, g_close_count);
    TEST_ASSERT_EQUAL(10, hid.owner_id);
}

void test_a_connection_without_a_token_is_refused(void) {
    TEST_ASSERT_EQUAL(-1, ws_input_core_on_connect(&core, conn_n(4), 40, NULL, 0)); /* the table is empty: only the token is wrong */
    TEST_ASSERT_EQUAL(-1, hid.owner_id);
    TEST_ASSERT_EQUAL(0, g_send_count);
}

void test_closed_connections_free_their_slots(void) {
    for (int round = 0; round < 10; round++) {
        TEST_ASSERT_EQUAL(0, connect_n(1, 10 + round, 100 + round));
        TEST_ASSERT_EQUAL(0, connect_n(2, 50 + round, 100 + round)); /* denied, but it takes a slot */
        ws_input_core_on_close(&core, 10 + round);
        ws_input_core_on_close(&core, 50 + round);
        TEST_ASSERT_EQUAL(-1, hid.owner_id);
    }
}

void test_a_failed_send_does_not_undo_the_registration(void) {
    g_send_result = -1;
    TEST_ASSERT_EQUAL(0, connect_n(1, 10, 0));
    TEST_ASSERT_EQUAL(10, hid.owner_id);
    g_send_result = 0;
    key_down(10, 0x04, 1);
    TEST_ASSERT_EQUAL(1, g_kbd_count); /* the controller works */
}

/* ---- frames ---- */
void test_a_frame_from_the_controller_reaches_the_sink(void) {
    connect_n(1, 10, 0);
    key_down(10, 0x04, 1);
    TEST_ASSERT_EQUAL(1, g_kbd_count);
    TEST_ASSERT_EQUAL_HEX8(0x04, g_kbd_last.keys[0]);
    TEST_ASSERT_EQUAL(0, g_close_count);
}

void test_a_frame_from_a_non_controller_closes_it_and_does_not_reach_the_sink(void) {
    connect_n(1, 10, 0);
    connect_n(2, 20, 0);
    g_close_count = 0;
    key_down(20, 0x04, 1);
    TEST_ASSERT_EQUAL(0, g_kbd_count);
    TEST_ASSERT_EQUAL(1, g_close_count);
    TEST_ASSERT_EQUAL_PTR(conn_n(2), g_closed[g_close_count - 1]);
    TEST_ASSERT_EQUAL(10, hid.owner_id);
}

void test_a_frame_from_an_unknown_fd_or_without_payload_is_ignored(void) {
    connect_n(1, 10, 0);
    key_down(99, 0x04, 1);
    ws_input_core_on_frame(&core, 10, NULL, 2, 1);
    TEST_ASSERT_EQUAL(0, g_kbd_count);
    TEST_ASSERT_EQUAL(0, g_close_count);
}

void test_invalid_and_rate_limited_messages_do_not_close(void) {
    connect_n(1, 10, 0);
    uint8_t bad[2] = {0x01, 0x01}; /* usage 0x01 is not a key */
    ws_input_core_on_frame(&core, 10, bad, 2, 1);
    uint8_t unknown[1] = {0x7F};
    ws_input_core_on_frame(&core, 10, unknown, 1, 1);
    TEST_ASSERT_EQUAL(0, g_close_count);
    TEST_ASSERT_EQUAL(10, hid.owner_id);
}

void test_abuse_releases_the_keys_and_closes(void) {
    connect_n(1, 10, 0);
    key_down(10, 0x04, 100);
    int closed_before_the_flood = g_close_count;
    uint8_t ka[1] = {0x04};
    for (int i = 0; i < 1200 && hid.owner_id != -1; i++) {
        ws_input_core_on_frame(&core, 10, ka, 1, 100);
    }
    TEST_ASSERT_EQUAL(-1, hid.owner_id);
    /* The close is requested by the very frame that was judged abuse, not by a later frame from a non-controller. */
    TEST_ASSERT_EQUAL(closed_before_the_flood + 1, g_close_count);
    TEST_ASSERT_EQUAL_PTR(conn_n(1), g_closed[g_close_count - 1]);
    TEST_ASSERT_EQUAL(-1, hid.owner_id);
    TEST_ASSERT_EQUAL_HEX8(0, g_kbd_last.keys[0]); /* the key was released */
}

void test_violation_by_the_controller_releases_everything_and_closes(void) {
    connect_n(1, 10, 0);
    key_down(10, 0x04, 1);
    g_close_count = 0;
    ws_input_core_on_violation(&core, 10);
    TEST_ASSERT_EQUAL(1, g_close_count);
    TEST_ASSERT_EQUAL_PTR(conn_n(1), g_closed[0]);
    TEST_ASSERT_EQUAL(-1, hid.owner_id);
    TEST_ASSERT_EQUAL_HEX8(0, g_kbd_last.keys[0]);
}

void test_violation_by_a_non_controller_closes_only_that_connection(void) {
    connect_n(1, 10, 0);
    connect_n(2, 20, 0);
    key_down(10, 0x04, 1);
    g_close_count = 0;
    int kbd_before = g_kbd_count;
    ws_input_core_on_violation(&core, 20);
    TEST_ASSERT_EQUAL(1, g_close_count);
    TEST_ASSERT_EQUAL_PTR(conn_n(2), g_closed[0]);
    TEST_ASSERT_EQUAL(10, hid.owner_id);
    TEST_ASSERT_EQUAL(kbd_before, g_kbd_count); /* the controller's key stays down */
    ws_input_core_on_violation(&core, 99);      /* unknown fd: nothing */
    TEST_ASSERT_EQUAL(1, g_close_count);
}

/* ---- close ---- */
void test_closing_the_controller_releases_everything_and_frees_the_slot(void) {
    connect_n(1, 10, 0);
    key_down(10, 0x04, 1);
    ws_input_core_on_close(&core, 10);
    TEST_ASSERT_EQUAL(-1, hid.owner_id);
    TEST_ASSERT_EQUAL_HEX8(0, g_kbd_last.keys[0]);
    TEST_ASSERT_EQUAL(0, connect_n(2, 20, 5)); /* the next connection becomes the controller */
    TEST_ASSERT_EQUAL(20, hid.owner_id);
}

void test_closing_a_denied_connection_does_not_touch_the_controllers_keys(void) {
    connect_n(1, 10, 0);
    connect_n(2, 20, 0);
    key_down(10, 0x04, 1);
    int kbd_before = g_kbd_count;
    ws_input_core_on_close(&core, 20);
    TEST_ASSERT_EQUAL(10, hid.owner_id);
    TEST_ASSERT_EQUAL(kbd_before, g_kbd_count);
    TEST_ASSERT_EQUAL_HEX8(0x04, g_kbd_last.keys[0]);
    ws_input_core_on_close(&core, 77); /* unknown fd: nothing */
    TEST_ASSERT_EQUAL(10, hid.owner_id);
}

/* ---- tick ---- */
void test_tick_runs_the_hold_timeout(void) {
    connect_n(1, 10, 0);
    key_down(10, 0x04, 100);
    ws_input_core_tick(&core, 3100); /* exactly the timeout */
    TEST_ASSERT_EQUAL_HEX8(0x04, g_kbd_last.keys[0]);
    ws_input_core_tick(&core, 3101);
    TEST_ASSERT_EQUAL_HEX8(0, g_kbd_last.keys[0]);
}

void test_usb_state_is_sent_to_every_connection_when_it_changes_and_only_then(void) {
    connect_n(1, 10, 0);
    connect_n(2, 20, 0);
    g_send_count = 0;
    ws_input_core_tick(&core, 100); /* still not ready: nothing changed */
    TEST_ASSERT_EQUAL(0, g_send_count);

    g_usb_ready = true;
    ws_input_core_tick(&core, 110);
    TEST_ASSERT_EQUAL(2, g_send_count);
    for (int i = 0; i < 2; i++) {
        TEST_ASSERT_EQUAL_HEX8(0x82, g_sent[i].data[0]);
        TEST_ASSERT_EQUAL_HEX8(1, g_sent[i].data[1]);
    }
    TEST_ASSERT_TRUE(g_sent[0].conn != g_sent[1].conn);

    g_send_count = 0;
    ws_input_core_tick(&core, 120); /* no change */
    TEST_ASSERT_EQUAL(0, g_send_count);

    g_usb_ready = false;
    ws_input_core_tick(&core, 130);
    TEST_ASSERT_EQUAL(2, g_send_count);
    TEST_ASSERT_EQUAL_HEX8(0, g_sent[0].data[1]);
}

void test_a_failed_usb_state_send_is_retried_on_the_next_tick(void) {
    connect_n(1, 10, 0);
    g_usb_ready = true;
    g_send_count = 0;
    g_send_result = -1;
    ws_input_core_tick(&core, 100);
    TEST_ASSERT_EQUAL(1, g_send_count);
    g_send_result = 0;
    g_send_count = 0;
    ws_input_core_tick(&core, 110);
    TEST_ASSERT_EQUAL(1, g_send_count); /* retried */
    TEST_ASSERT_EQUAL_HEX8(1, g_sent[0].data[1]);
    g_send_count = 0;
    ws_input_core_tick(&core, 120);
    TEST_ASSERT_EQUAL(0, g_send_count); /* and then quiet */
}

void test_no_usb_ready_callback_means_not_ready(void) {
    ws_input_core_config_t cc = base_config();
    cc.usb_ready = NULL;
    TEST_ASSERT_EQUAL(0, ws_input_core_init(&core, &cc, &hid));
    TEST_ASSERT_EQUAL(0, connect_n(1, 10, 0));
    TEST_ASSERT_EQUAL_HEX8(0, g_sent[1].data[1]);
    g_send_count = 0;
    ws_input_core_tick(&core, 100);
    TEST_ASSERT_EQUAL(0, g_send_count);
}

void test_session_is_rechecked_at_the_interval_and_not_before(void) {
    connect_n(1, 10, 0);
    key_down(10, 0x04, 4900); /* the hold timeout (3 s) must not be what releases it */
    g_session_valid = false;
    ws_input_core_tick(&core, 4999);
    TEST_ASSERT_EQUAL(0, g_validate_calls);
    TEST_ASSERT_EQUAL(0, g_close_count);
    ws_input_core_tick(&core, 5000);
    TEST_ASSERT_EQUAL(1, g_validate_calls);
    TEST_ASSERT_EQUAL(1, g_close_count);
    TEST_ASSERT_EQUAL_PTR(conn_n(1), g_closed[0]);
    TEST_ASSERT_EQUAL(-1, hid.owner_id);          /* the controller's keys are released right away */
    TEST_ASSERT_EQUAL_HEX8(0, g_kbd_last.keys[0]);
}

void test_a_valid_session_stays_and_the_interval_restarts(void) {
    connect_n(1, 10, 0);
    ws_input_core_tick(&core, 5000);
    TEST_ASSERT_EQUAL(1, g_validate_calls);
    TEST_ASSERT_EQUAL(0, g_close_count);
    ws_input_core_tick(&core, 9999);
    TEST_ASSERT_EQUAL(1, g_validate_calls);
    ws_input_core_tick(&core, 10000);
    TEST_ASSERT_EQUAL(2, g_validate_calls);
    TEST_ASSERT_EQUAL(10, hid.owner_id);
}

void test_an_invalid_non_controller_session_is_closed_without_touching_the_controller(void) {
    g_per_token = true;
    g_session_valid_by_token[1] = true;  /* token_n(1) */
    g_session_valid_by_token[2] = false; /* token_n(2) */
    connect_n(1, 10, 0);
    connect_n(2, 20, 0);
    key_down(10, 0x04, 4900);
    g_close_count = 0;
    ws_input_core_tick(&core, 5000);
    TEST_ASSERT_EQUAL(2, g_validate_calls);
    TEST_ASSERT_EQUAL(1, g_close_count);
    TEST_ASSERT_EQUAL_PTR(conn_n(2), g_closed[0]);
    TEST_ASSERT_EQUAL(10, hid.owner_id);
    TEST_ASSERT_EQUAL_HEX8(0x04, g_kbd_last.keys[0]);
}

void test_the_session_check_survives_the_clock_going_back(void) {
    connect_n(1, 10, 0);
    ws_input_core_tick(&core, 5000);
    TEST_ASSERT_EQUAL(1, g_validate_calls);
    ws_input_core_tick(&core, 100); /* the clock went back: no huge unsigned difference, no check */
    TEST_ASSERT_EQUAL(1, g_validate_calls);
    ws_input_core_tick(&core, 5099);
    TEST_ASSERT_EQUAL(1, g_validate_calls);
    ws_input_core_tick(&core, 5100);
    TEST_ASSERT_EQUAL(2, g_validate_calls);
}

void test_the_session_timer_starts_with_the_first_connection(void) {
    connect_n(1, 10, 10000);
    ws_input_core_tick(&core, 14999);
    TEST_ASSERT_EQUAL(0, g_validate_calls);
    ws_input_core_tick(&core, 15000);
    TEST_ASSERT_EQUAL(1, g_validate_calls);
}

void test_without_a_validator_nothing_is_closed(void) {
    ws_input_core_config_t cc = base_config();
    cc.io_validate_session = NULL;
    TEST_ASSERT_EQUAL(0, ws_input_core_init(&core, &cc, &hid));
    connect_n(1, 10, 0);
    ws_input_core_tick(&core, 6000);
    TEST_ASSERT_EQUAL(0, g_close_count);
}

void test_usb_mounted_releases_everything(void) {
    connect_n(1, 10, 0);
    key_down(10, 0x04, 1);
    ws_input_core_on_usb_mounted(&core);
    TEST_ASSERT_EQUAL_HEX8(0, g_kbd_last.keys[0]);
    TEST_ASSERT_EQUAL(10, hid.owner_id); /* the connection stays the controller */
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_init_validates_arguments);
    RUN_TEST(test_check_frame_boundaries);
    RUN_TEST(test_first_connection_becomes_controller_and_gets_granted_then_usb_state);
    RUN_TEST(test_usb_state_zero_when_the_target_is_not_ready);
    RUN_TEST(test_second_connection_is_denied_and_closed_and_the_controller_is_unchanged);
    RUN_TEST(test_connection_table_full_and_duplicate_fd_are_refused_without_side_effects);
    RUN_TEST(test_a_connection_without_a_token_is_refused);
    RUN_TEST(test_closed_connections_free_their_slots);
    RUN_TEST(test_a_failed_send_does_not_undo_the_registration);
    RUN_TEST(test_a_frame_from_the_controller_reaches_the_sink);
    RUN_TEST(test_a_frame_from_a_non_controller_closes_it_and_does_not_reach_the_sink);
    RUN_TEST(test_a_frame_from_an_unknown_fd_or_without_payload_is_ignored);
    RUN_TEST(test_invalid_and_rate_limited_messages_do_not_close);
    RUN_TEST(test_abuse_releases_the_keys_and_closes);
    RUN_TEST(test_violation_by_the_controller_releases_everything_and_closes);
    RUN_TEST(test_violation_by_a_non_controller_closes_only_that_connection);
    RUN_TEST(test_closing_the_controller_releases_everything_and_frees_the_slot);
    RUN_TEST(test_closing_a_denied_connection_does_not_touch_the_controllers_keys);
    RUN_TEST(test_tick_runs_the_hold_timeout);
    RUN_TEST(test_usb_state_is_sent_to_every_connection_when_it_changes_and_only_then);
    RUN_TEST(test_a_failed_usb_state_send_is_retried_on_the_next_tick);
    RUN_TEST(test_no_usb_ready_callback_means_not_ready);
    RUN_TEST(test_session_is_rechecked_at_the_interval_and_not_before);
    RUN_TEST(test_a_valid_session_stays_and_the_interval_restarts);
    RUN_TEST(test_an_invalid_non_controller_session_is_closed_without_touching_the_controller);
    RUN_TEST(test_the_session_check_survives_the_clock_going_back);
    RUN_TEST(test_the_session_timer_starts_with_the_first_connection);
    RUN_TEST(test_without_a_validator_nothing_is_closed);
    RUN_TEST(test_usb_mounted_releases_everything);
    return UNITY_END();
}
