#include "unity.h"
#include "ws_input_core.h"
#include <string.h>

/* Fixed-seed fuzzing of ws_input_core (with the real hid_logic): random connects, closes, frames (valid and garbage),
 * violations, ticks, USB state changes, session losses and clock jumps. Under ASan/UBSan (test/host/CMakeLists.txt).
 *
 * Invariants:
 *  - the controller is -1 or the fd of a registered connection (never a stranger, never two);
 *  - every message sent is one of the three server messages, with the exact length;
 *  - io_request_close only ever targets a connection that exists (an fd that was used);
 *  - after the controller is gone and ticks run, nothing stays pressed.
 */

#define FUZZ_STEPS 200000
#define NFD 6

static uint32_t seed = 12345;
static uint32_t rnd(void) {
    seed = (seed * 1103515245u + 12345u) & 0x7fffffffu;
    return seed >> 8;
}

static bool g_connected[NFD];
static bool g_session_ok = true;
static bool g_usb = false;
static bool g_bad_message;
static bool g_bad_close;
static int g_pressed_keys; /* what the target currently sees: number of keys in the last keyboard report */
static hid_kbd_report_t g_last_kbd;
static bool g_kbd_seen;

static int fd_of(ws_input_conn_t c) { return (int)((intptr_t)c - 1000) ; }

static int mock_send(void *u, ws_input_conn_t conn, const uint8_t *d, size_t len) {
    (void)u; (void)conn;
    bool ok = (len == 1 && d[0] == 0x80) || (len == 2 && d[0] == 0x81 && d[1] == 1) ||
              (len == 2 && d[0] == 0x82 && (d[1] == 0 || d[1] == 1));
    if (!ok) g_bad_message = true;
    return (rnd() % 8 == 0) ? -1 : 0; /* the send queue is sometimes full */
}
static void mock_close(void *u, ws_input_conn_t conn) {
    (void)u;
    int idx = fd_of(conn);
    if (idx < 0 || idx >= NFD) { g_bad_close = true; return; }
}
static bool mock_validate(void *u, const uint8_t t[32]) { (void)u; (void)t; return g_session_ok; }
static bool mock_usb(void *u) { (void)u; return g_usb; }

static bool sink_ready(void *c) { (void)c; return true; }
static bool sink_kbd(void *c, const hid_kbd_report_t *r) {
    (void)c;
    g_last_kbd = *r;
    g_kbd_seen = true;
    return (rnd() % 16) != 0; /* sometimes refused: hid_logic re-sends */
}
static bool sink_abs(void *c, const hid_abs_report_t *r) { (void)c; (void)r; return true; }
static bool sink_rel(void *c, const hid_rel_report_t *r) { (void)c; (void)r; return true; }

static void test_fuzz(void) {
    hid_logic_t hid;
    hid_logic_config_t hc = { .hold_timeout_ms = 3000, .rate_per_sec = 400, .rate_burst = 100 };
    hid_sink_t sink = { .ctx = NULL, .ready = sink_ready, .send_keyboard = sink_kbd, .send_abs = sink_abs, .send_rel = sink_rel };
    hid_logic_init(&hid, &hc, &sink);

    ws_input_core_t core;
    ws_input_core_config_t cc = { .max_clients = 3, .session_check_interval_ms = 5000, .io_send = mock_send,
                                  .io_request_close = mock_close, .io_validate_session = mock_validate,
                                  .usb_ready = mock_usb, .user_data = NULL };
    TEST_ASSERT_EQUAL(0, ws_input_core_init(&core, &cc, &hid));

    uint64_t now = 1000;
    uint8_t token[32] = {0};
    for (int step = 0; step < FUZZ_STEPS; step++) {
        now += rnd() % 30;
        if (rnd() % 20000 == 0) now = now > 800 ? now - 800 : now; /* the clock jumps back */
        int fd = (int)(rnd() % NFD);
        switch (rnd() % 8) {
        case 0:
            if (!g_connected[fd]) {
                if (ws_input_core_on_connect(&core, (ws_input_conn_t)(intptr_t)(1000 + fd), fd, token, now) == 0) {
                    g_connected[fd] = true;
                }
            }
            break;
        case 1:
            if (g_connected[fd]) {
                ws_input_core_on_close(&core, fd);
                g_connected[fd] = false;
            }
            break;
        case 2: { /* a valid-looking message */
            uint8_t m[7];
            size_t len = 2;
            switch (rnd() % 5) {
            case 0: m[0] = 0x01; m[1] = (uint8_t)(0x04 + rnd() % 10); break;
            case 1: m[0] = 0x02; m[1] = (uint8_t)(0x04 + rnd() % 10); break;
            case 2: m[0] = (rnd() % 4) ? 0x04 : 0x03; len = 1; break;
            case 3: m[0] = 0x10; for (int i = 1; i < 7; i++) m[i] = (uint8_t)rnd(); len = 7; break;
            default: m[0] = 0x11; for (int i = 1; i < 7; i++) m[i] = (uint8_t)rnd(); len = 7; break;
            }
            if (g_connected[fd]) ws_input_core_on_frame(&core, fd, m, len, now);
            break;
        }
        case 3: { /* garbage */
            uint8_t m[16];
            size_t len = 1 + rnd() % 16;
            for (size_t i = 0; i < len; i++) m[i] = (uint8_t)rnd();
            if (g_connected[fd]) ws_input_core_on_frame(&core, fd, m, len, now);
            break;
        }
        case 4:
            if (g_connected[fd]) ws_input_core_on_violation(&core, fd);
            break;
        case 5:
            if (rnd() % 4 == 0) g_usb = !g_usb;
            if (rnd() % 50 == 0) g_session_ok = !g_session_ok;
            ws_input_core_tick(&core, now);
            break;
        case 6:
            if (rnd() % 20 == 0) ws_input_core_on_usb_mounted(&core);
            break;
        default:
            ws_input_core_tick(&core, now);
            break;
        }

        /* invariants */
        if (hid.owner_id != -1) {
            TEST_ASSERT_TRUE_MESSAGE(hid.owner_id >= 0 && hid.owner_id < NFD, "controller is not a known fd");
            TEST_ASSERT_TRUE_MESSAGE(ws_clients_find(&core.clients, hid.owner_id) >= 0, "controller is not registered");
        }
        TEST_ASSERT_TRUE_MESSAGE(ws_clients_count(&core.clients) <= 3, "too many connections");
        TEST_ASSERT_FALSE_MESSAGE(g_bad_message, "a server message with a wrong shape was sent");
        TEST_ASSERT_FALSE_MESSAGE(g_bad_close, "close of an unknown connection");
    }

    /* Everything ends: the controller goes away (or the session is lost), ticks run, nothing stays pressed. */
    for (int fd = 0; fd < NFD; fd++) {
        if (g_connected[fd]) ws_input_core_on_close(&core, fd);
    }
    TEST_ASSERT_EQUAL(-1, hid.owner_id);
    for (int t = 0; t < 50; t++) ws_input_core_tick(&core, now + (uint64_t)t * 20);
    if (g_kbd_seen) {
        g_pressed_keys = 0;
        for (int i = 0; i < 6; i++) if (g_last_kbd.keys[i]) g_pressed_keys++;
        TEST_ASSERT_EQUAL(0, g_pressed_keys);
        TEST_ASSERT_EQUAL(0, g_last_kbd.modifiers);
    }
}

void setUp(void) {}
void tearDown(void) {}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_fuzz);
    return UNITY_END();
}
