#include "unity.h"
#include "hid_logic.h"
#include <string.h>
#include <stdlib.h>

#define MAX_REPORTS 500

typedef struct {
    hid_kbd_report_t kbd[MAX_REPORTS];
    size_t kbd_count;

    hid_abs_report_t abs[MAX_REPORTS];
    size_t abs_count;

    hid_rel_report_t rel[MAX_REPORTS];
    size_t rel_count;

    bool is_ready;
    int fail_kbd_on;
    int fail_abs_on;
    int fail_rel_on;
    int fail_rel_next; /* fail the next N send_rel calls */
} mock_sink_ctx_t;

static mock_sink_ctx_t ctx;

static bool mock_ready(void *c) {
    return ((mock_sink_ctx_t *)c)->is_ready;
}

static bool mock_send_kbd(void *c, const hid_kbd_report_t *r) {
    mock_sink_ctx_t *m = (mock_sink_ctx_t *)c;
    if (m->fail_kbd_on > 0 && m->kbd_count + 1 == (size_t)m->fail_kbd_on) return false;
    if (m->kbd_count < MAX_REPORTS) {
        m->kbd[m->kbd_count++] = *r;
    }
    return true;
}

static bool mock_send_abs(void *c, const hid_abs_report_t *r) {
    mock_sink_ctx_t *m = (mock_sink_ctx_t *)c;
    if (m->fail_abs_on > 0 && m->abs_count + 1 == (size_t)m->fail_abs_on) return false;
    if (m->abs_count < MAX_REPORTS) {
        m->abs[m->abs_count++] = *r;
    }
    return true;
}

static bool mock_send_rel(void *c, const hid_rel_report_t *r) {
    mock_sink_ctx_t *m = (mock_sink_ctx_t *)c;
    if (m->fail_rel_next > 0) { m->fail_rel_next--; return false; }
    if (m->fail_rel_on > 0 && m->rel_count + 1 == (size_t)m->fail_rel_on) return false;
    if (m->rel_count < MAX_REPORTS) {
        m->rel[m->rel_count++] = *r;
    }
    return true;
}

static hid_sink_t sink = {
    .ctx = &ctx,
    .ready = mock_ready,
    .send_keyboard = mock_send_kbd,
    .send_abs = mock_send_abs,
    .send_rel = mock_send_rel
};

static hid_logic_t logic;
static uint64_t now;

void setUp(void) {
    memset(&ctx, 0, sizeof(ctx));
    ctx.is_ready = true;
    now = 1000;
    hid_logic_init(&logic, NULL, &sink);
    hid_logic_acquire(&logic, 1);
}

void tearDown(void) {}

static hid_result_t send_msg(const uint8_t *msg, size_t len) {
    return hid_logic_on_message(&logic, 1, msg, len, now);
}

void test_basic_keys_and_rollover(void) {
    uint8_t msg1[] = {0x01, 0x04}; // A
    TEST_ASSERT_EQUAL(HID_RESULT_OK, send_msg(msg1, 2));
    TEST_ASSERT_EQUAL(1, ctx.kbd_count);
    TEST_ASSERT_EQUAL(0, ctx.kbd[0].modifiers);
    TEST_ASSERT_EQUAL(0x04, ctx.kbd[0].keys[0]);
    TEST_ASSERT_EQUAL(0x00, ctx.kbd[0].keys[1]);

    // modifiers
    uint8_t msg2[] = {0x01, 0xE0}; // LCtrl
    TEST_ASSERT_EQUAL(HID_RESULT_OK, send_msg(msg2, 2));
    TEST_ASSERT_EQUAL(2, ctx.kbd_count);
    TEST_ASSERT_EQUAL(0x01, ctx.kbd[1].modifiers);
    TEST_ASSERT_EQUAL(0x04, ctx.kbd[1].keys[0]);

    uint8_t msg_e1[] = {0x01, 0xE1}; // LShift
    TEST_ASSERT_EQUAL(HID_RESULT_OK, send_msg(msg_e1, 2));
    TEST_ASSERT_EQUAL(3, ctx.kbd_count);
    TEST_ASSERT_EQUAL(0x03, ctx.kbd[2].modifiers);

    // Roll over: 6 normal keys + 2 modifiers
    for(uint8_t i = 0x05; i <= 0x09; i++) {
        uint8_t msg[] = {0x01, i};
        send_msg(msg, 2);
    }

    // Now 6 normal keys are pressed (0x04 to 0x09)
    size_t c = ctx.kbd_count - 1;
    TEST_ASSERT_EQUAL(0x03, ctx.kbd[c].modifiers);
    TEST_ASSERT_EQUAL(0x04, ctx.kbd[c].keys[0]);
    TEST_ASSERT_EQUAL(0x09, ctx.kbd[c].keys[5]);

    // 7th normal key -> Rollover
    uint8_t msg3[] = {0x01, 0x0A};
    send_msg(msg3, 2);
    c = ctx.kbd_count - 1;
    TEST_ASSERT_EQUAL(0x03, ctx.kbd[c].modifiers);
    for(int i=0; i<6; i++) {
        TEST_ASSERT_EQUAL(0x01, ctx.kbd[c].keys[i]);
    }

    // Release 7th normal key
    uint8_t msg4[] = {0x02, 0x0A};
    send_msg(msg4, 2);
    c = ctx.kbd_count - 1;
    TEST_ASSERT_EQUAL(0x03, ctx.kbd[c].modifiers);
    TEST_ASSERT_EQUAL(0x04, ctx.kbd[c].keys[0]);
    TEST_ASSERT_EQUAL(0x09, ctx.kbd[c].keys[5]);
}

void test_key_boundaries(void) {
    uint8_t m1[] = {0x01, 0x03}; // Invalid
    TEST_ASSERT_EQUAL(HID_RESULT_INVALID, send_msg(m1, 2));
    uint8_t m2[] = {0x01, 0xA5}; // Invalid
    TEST_ASSERT_EQUAL(HID_RESULT_INVALID, send_msg(m2, 2));
    uint8_t m3[] = {0x01, 0xDF}; // Invalid
    TEST_ASSERT_EQUAL(HID_RESULT_INVALID, send_msg(m3, 2));
    uint8_t m4[] = {0x01, 0xE8}; // Invalid
    TEST_ASSERT_EQUAL(HID_RESULT_INVALID, send_msg(m4, 2));

    uint8_t m5[] = {0x01, 0x04}; // Valid
    TEST_ASSERT_EQUAL(HID_RESULT_OK, send_msg(m5, 2));
    uint8_t m6[] = {0x01, 0xA4}; // Valid
    TEST_ASSERT_EQUAL(HID_RESULT_OK, send_msg(m6, 2));
    uint8_t m7[] = {0x01, 0xE7}; // Valid
    TEST_ASSERT_EQUAL(HID_RESULT_OK, send_msg(m7, 2));
}

void test_absolute_pointer(void) {
    uint8_t m[] = {0x10, 0x01, 0xFF, 0x7F, 0x00, 0x80, 0x80}; // btn 1, x=32767, y=32768 (clamped to 32767), wheel=-128 (clamped to -127)
    TEST_ASSERT_EQUAL(HID_RESULT_OK, send_msg(m, 7));
    TEST_ASSERT_EQUAL(1, ctx.abs_count);
    TEST_ASSERT_EQUAL(1, ctx.abs[0].buttons);
    TEST_ASSERT_EQUAL(32767, ctx.abs[0].x);
    TEST_ASSERT_EQUAL(32767, ctx.abs[0].y);
    TEST_ASSERT_EQUAL(-127, ctx.abs[0].wheel);
}

void test_relative_pointer_splits_and_saturation(void) {
    // dx = 300, dy = -300
    uint8_t m[] = {0x11, 0x02, 0x2C, 0x01, 0xD4, 0xFE, 0x00};
    TEST_ASSERT_EQUAL(HID_RESULT_OK, send_msg(m, 7));
    // Should split into: (127, -127), (127, -127), (46, -46)
    TEST_ASSERT_EQUAL(3, ctx.rel_count);
    TEST_ASSERT_EQUAL(127, ctx.rel[0].dx);
    TEST_ASSERT_EQUAL(-127, ctx.rel[0].dy);
    TEST_ASSERT_EQUAL(127, ctx.rel[1].dx);
    TEST_ASSERT_EQUAL(-127, ctx.rel[1].dy);
    TEST_ASSERT_EQUAL(46, ctx.rel[2].dx);
    TEST_ASSERT_EQUAL(-46, ctx.rel[2].dy);

    // Saturation testing
    // Sending huge dx
    for(int i=0; i<150; i++) {
        uint8_t mh[] = {0x11, 0x00, 0xFF, 0x7F, 0x00, 0x00, 0x00}; // dx=32767
        // Mock sink is not ready
        ctx.is_ready = false;
        send_msg(mh, 7);
        // Provide enough tokens for the rate limit
        now += 1000;
    }
    // Now logic accumulated > 32767. It should clamp to 32767.
    ctx.is_ready = true;
    // At most HID_REL_MAX_REPORTS_PER_CALL reports per call: drain over several ticks.
    long total_dx = 0;
    size_t prev = ctx.rel_count;
    for (int t = 0; t < 100; t++) {
        hid_logic_tick(&logic, now);
        TEST_ASSERT_TRUE(ctx.rel_count - prev <= HID_REL_MAX_REPORTS_PER_CALL);
        for (size_t i = prev; i < ctx.rel_count; i++) total_dx += ctx.rel[i].dx;
        if (ctx.rel_count == prev) break;
        prev = ctx.rel_count;
    }
    TEST_ASSERT_EQUAL(32767, total_dx);
}

void test_relative_cap_per_call(void) {
    uint8_t mh[] = {0x11, 0x00, 0xFF, 0x7F, 0x00, 0x00, 0x00}; // dx=32767 in one message
    TEST_ASSERT_EQUAL(HID_RESULT_OK, send_msg(mh, 7));
    TEST_ASSERT_EQUAL(HID_REL_MAX_REPORTS_PER_CALL, ctx.rel_count); // not all 258 at once
    hid_logic_tick(&logic, now);
    TEST_ASSERT_EQUAL(2 * HID_REL_MAX_REPORTS_PER_CALL, ctx.rel_count); // the rest waits for the next tick
}

void test_relative_button_change_survives_sink_failure(void) {
    // Button press without movement; the sink refuses it. The state must be re-sent on the next tick.
    uint8_t press[] = {0x11, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00};
    ctx.fail_rel_next = 1;
    TEST_ASSERT_EQUAL(HID_RESULT_OK, send_msg(press, 7));
    TEST_ASSERT_EQUAL(0, ctx.rel_count);
    hid_logic_tick(&logic, now + 1);
    TEST_ASSERT_EQUAL(1, ctx.rel_count);
    TEST_ASSERT_EQUAL(1, ctx.rel[0].buttons);

    // The release must not be lost either (a lost release leaves the button pressed on the target).
    uint8_t rel[] = {0x11, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    ctx.fail_rel_next = 1;
    TEST_ASSERT_EQUAL(HID_RESULT_OK, hid_logic_on_message(&logic, 1, rel, 7, now + 2));
    TEST_ASSERT_EQUAL(1, ctx.rel_count);
    hid_logic_tick(&logic, now + 3);
    TEST_ASSERT_EQUAL(2, ctx.rel_count);
    TEST_ASSERT_EQUAL(0, ctx.rel[1].buttons);
    // And nothing is re-sent once it got through.
    hid_logic_tick(&logic, now + 4);
    TEST_ASSERT_EQUAL(2, ctx.rel_count);
}

void test_rate_tokens_survive_a_very_long_idle(void) {
    uint8_t k[] = {0x01, 0x04};
    TEST_ASSERT_EQUAL(HID_RESULT_OK, send_msg(k, 2));
    now += 6000000000ULL; // ~69 days idle: add_tokens = 2.4e9 does not fit int32_t and used to wrap to a negative bucket
    uint8_t ku[] = {0x02, 0x04};
    TEST_ASSERT_EQUAL(HID_RESULT_OK, send_msg(ku, 2));
    uint8_t kd[] = {0x01, 0x05};
    TEST_ASSERT_EQUAL(HID_RESULT_OK, send_msg(kd, 2));
}

void test_switch_to_abs_drops_stale_relative_movement(void) {
    // Relative movement is queued (sink not ready), then the client switches to the absolute pointer with a button down.
    uint8_t mr[] = {0x11, 0x00, 0x0A, 0x00, 0x00, 0x00, 0x00};
    ctx.is_ready = false;
    send_msg(mr, 7);
    uint8_t ma[] = {0x10, 0x01, 0x10, 0x00, 0x10, 0x00, 0x00};
    send_msg(ma, 7);
    ctx.is_ready = true;
    hid_logic_tick(&logic, now);
    hid_logic_tick(&logic, now);
    for (size_t i = 0; i < ctx.rel_count; i++) {
        TEST_ASSERT_EQUAL(0, ctx.rel[i].buttons); // the absolute button state never leaks to the relative interface
    }
    TEST_ASSERT_TRUE(ctx.abs_count >= 1);
    TEST_ASSERT_EQUAL(1, ctx.abs[ctx.abs_count - 1].buttons);
}

void test_pointer_type_switch(void) {
    uint8_t mr[] = {0x11, 0x01, 0x0A, 0x00, 0x00, 0x00, 0x00}; // REL, btn1
    TEST_ASSERT_EQUAL(HID_RESULT_OK, send_msg(mr, 7));
    TEST_ASSERT_EQUAL(1, ctx.rel_count);
    TEST_ASSERT_EQUAL(1, ctx.rel[0].buttons);

    // Switch to ABS
    uint8_t ma[] = {0x10, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00}; // ABS, btn1
    TEST_ASSERT_EQUAL(HID_RESULT_OK, send_msg(ma, 7));
    // Should have sent REL with btn=0 first
    TEST_ASSERT_EQUAL(2, ctx.rel_count);
    TEST_ASSERT_EQUAL(0, ctx.rel[1].buttons);
    TEST_ASSERT_EQUAL(0, ctx.rel[1].dx);

    // Then ABS with btn1
    TEST_ASSERT_EQUAL(1, ctx.abs_count);
    TEST_ASSERT_EQUAL(1, ctx.abs[0].buttons);

    // Switch to REL again
    uint8_t mr2[] = {0x11, 0x01, 0x0A, 0x00, 0x00, 0x00, 0x00};
    TEST_ASSERT_EQUAL(HID_RESULT_OK, send_msg(mr2, 7));

    // Should have sent ABS with btn=0 first
    TEST_ASSERT_EQUAL(2, ctx.abs_count);
    TEST_ASSERT_EQUAL(0, ctx.abs[1].buttons);

    // Then REL with btn1
    TEST_ASSERT_EQUAL(3, ctx.rel_count);
    TEST_ASSERT_EQUAL(1, ctx.rel[2].buttons);
    TEST_ASSERT_EQUAL(10, ctx.rel[2].dx);
}

void test_pointer_type_switch_failed_retransmit(void) {
    uint8_t mr[] = {0x11, 0x01, 0x0A, 0x00, 0x00, 0x00, 0x00}; // REL, btn1
    TEST_ASSERT_EQUAL(HID_RESULT_OK, send_msg(mr, 7));

    // Switch to ABS, but sink will fail to send the old REL (btn=0)
    ctx.fail_rel_on = 2; // Fail the 2nd REL msg
    uint8_t ma[] = {0x10, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00}; // ABS, btn1
    TEST_ASSERT_EQUAL(HID_RESULT_OK, send_msg(ma, 7));

    // ABS shouldn't be sent yet
    TEST_ASSERT_EQUAL(0, ctx.abs_count);
    TEST_ASSERT_EQUAL(1, ctx.rel_count);

    // Now tick, allow rel to send
    ctx.fail_rel_on = 0;
    hid_logic_tick(&logic, now);

    // Should have sent REL btn=0, then ABS btn1
    TEST_ASSERT_EQUAL(2, ctx.rel_count);
    TEST_ASSERT_EQUAL(0, ctx.rel[1].buttons);
    TEST_ASSERT_EQUAL(1, ctx.abs_count);
    TEST_ASSERT_EQUAL(1, ctx.abs[0].buttons);
}

void test_retransmit_and_ready_flag(void) {
    uint8_t mk[] = {0x01, 0x04};
    ctx.fail_kbd_on = 1;
    send_msg(mk, 2);
    TEST_ASSERT_EQUAL(0, ctx.kbd_count);

    ctx.fail_kbd_on = 0;
    hid_logic_tick(&logic, now);
    TEST_ASSERT_EQUAL(1, ctx.kbd_count);
    TEST_ASSERT_EQUAL(0x04, ctx.kbd[0].keys[0]);

    ctx.is_ready = false;
    uint8_t mk2[] = {0x02, 0x04};
    send_msg(mk2, 2);
    TEST_ASSERT_EQUAL(1, ctx.kbd_count); // Didn't send

    ctx.is_ready = true;
    hid_logic_tick(&logic, now);
    TEST_ASSERT_EQUAL(2, ctx.kbd_count); // Sent empty keyboard
    TEST_ASSERT_EQUAL(0x00, ctx.kbd[1].keys[0]);
}

void test_rate_limiting_and_bypass(void) {
    // We have 100 burst tokens.
    // Send 100 key downs.
    for(int i=0; i<100; i++) {
        uint8_t m[] = {0x01, 0x04};
        TEST_ASSERT_EQUAL(HID_RESULT_OK, send_msg(m, 2));
    }
    // 101st should rate limit
    uint8_t m1[] = {0x01, 0x04};
    TEST_ASSERT_EQUAL(HID_RESULT_RATE_LIMITED, send_msg(m1, 2));

    // KEY_UP should bypass rate limit, but it uses token if available. Here 0 tokens, so bypasses.
    uint8_t m2[] = {0x02, 0x04};
    TEST_ASSERT_EQUAL(HID_RESULT_OK, send_msg(m2, 2));

    // RELEASE_ALL bypasses
    uint8_t m3[] = {0x03};
    TEST_ASSERT_EQUAL(HID_RESULT_OK, send_msg(m3, 1));

    // Changing button pointer should bypass
    // We are currently ABS or REL? No pointer yet, default NONE. Send REL with btn=1
    uint8_t m4[] = {0x11, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00};
    TEST_ASSERT_EQUAL(HID_RESULT_OK, send_msg(m4, 7)); // Bypassed because btn changed from 0 to 1

    // Same button should rate limit
    uint8_t m5[] = {0x11, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00};
    TEST_ASSERT_EQUAL(HID_RESULT_RATE_LIMITED, send_msg(m5, 7));

    // Wait and refill tokens
    now += 1000;
    // Now we should have 400 new tokens (capped at burst=100)
    TEST_ASSERT_EQUAL(HID_RESULT_OK, send_msg(m5, 7));
}

void test_rate_limiting_abuse(void) {
    // Drop 1001 KEEPALIVE messages within 5 seconds
    // Exhaust tokens first
    for(int i=0; i<100; i++) {
        uint8_t m[] = {0x04};
        send_msg(m, 1);
    }

    for(int i=0; i<1000; i++) {
        uint8_t m[] = {0x04};
        hid_result_t res = send_msg(m, 1);
        if (i < 1000) {
            TEST_ASSERT_EQUAL(HID_RESULT_RATE_LIMITED, res);
        }
    }

    uint8_t m[] = {0x04};
    TEST_ASSERT_EQUAL(HID_RESULT_ABUSE, send_msg(m, 1));
}

void test_rate_limiting_abuse_bypass_counts_as_abuse(void) {
    // Bypass events (KEY_UP) without tokens should count towards ABUSE drops
    for(int i=0; i<100; i++) {
        uint8_t m[] = {0x04}; // exhaust tokens
        send_msg(m, 1);
    }

    for(int i=0; i<1000; i++) {
        uint8_t m[] = {0x02, 0x04}; // KEY_UP
        hid_result_t res = send_msg(m, 2);
        TEST_ASSERT_EQUAL(HID_RESULT_OK, res); // Bypass succeeds
    }

    uint8_t m[] = {0x02, 0x04};
    // 1001st drop
    TEST_ASSERT_EQUAL(HID_RESULT_ABUSE, send_msg(m, 2));
}

void test_owner_logic(void) {
    uint8_t mk[] = {0x01, 0x04};
    TEST_ASSERT_EQUAL(HID_RESULT_NOT_OWNER, hid_logic_on_message(&logic, 2, mk, 2, now));
    TEST_ASSERT_EQUAL(HID_RESULT_OK, hid_logic_on_message(&logic, 1, mk, 2, now));

    // Acquire when owned by 1
    TEST_ASSERT_FALSE(hid_logic_acquire(&logic, 2));

    // Non-owner release shouldn't do anything
    hid_logic_release_owner(&logic, 2);
    TEST_ASSERT_EQUAL(HID_RESULT_NOT_OWNER, hid_logic_on_message(&logic, 2, mk, 2, now));

    // Owner release
    hid_logic_release_owner(&logic, 1);

    // Release should have triggered release_all
    TEST_ASSERT_EQUAL(0, logic.modifiers);

    TEST_ASSERT_TRUE(hid_logic_acquire(&logic, 2));
    TEST_ASSERT_EQUAL(HID_RESULT_OK, hid_logic_on_message(&logic, 2, mk, 2, now));
}

void test_hold_timeout(void) {
    uint8_t mk[] = {0x01, 0x04};
    send_msg(mk, 2);
    TEST_ASSERT_EQUAL(1, logic.num_keys);

    now += 2000;
    hid_logic_tick(&logic, now);
    TEST_ASSERT_EQUAL(1, logic.num_keys); // Still pressed

    now += 1500; // 3500ms > 3000ms
    hid_logic_tick(&logic, now);
    TEST_ASSERT_EQUAL(0, logic.num_keys); // Released due to timeout

    // Last kbd report should be all released
    TEST_ASSERT_EQUAL(0x00, ctx.kbd[ctx.kbd_count-1].keys[0]);
}

void test_usb_mounted(void) {
    uint8_t mk[] = {0x01, 0x04};
    send_msg(mk, 2);
    uint8_t mr[] = {0x11, 0x01, 0x64, 0x00, 0x00, 0x00, 0x00}; // dx=100
    // not ready
    ctx.is_ready = false;
    send_msg(mr, 7);

    TEST_ASSERT_EQUAL(100, logic.rel_dx);

    hid_logic_on_usb_mounted(&logic);

    TEST_ASSERT_EQUAL(0, logic.num_keys);
    TEST_ASSERT_EQUAL(0, logic.rel_dx);
}

void test_time_reversing(void) {
    now = 5000;
    hid_logic_tick(&logic, now);

    now = 1000; // Clock backwards
    hid_logic_tick(&logic, now); // Should not assert or break

    uint8_t mk[] = {0x01, 0x04};
    TEST_ASSERT_EQUAL(HID_RESULT_OK, send_msg(mk, 2));

    // Timeout should work after time reset
    now += 4000;
    hid_logic_tick(&logic, now);
    TEST_ASSERT_EQUAL(0, logic.num_keys);
}

/* ---- tests added to kill surviving mutants (see the PR's mutation table) ---- */
static hid_result_t key_msg(uint8_t type, uint8_t usage) {
    uint8_t m[2] = {type, usage};
    return send_msg(m, 2);
}

void test_key_release_keeps_order_and_modifier_bits(void) {
    key_msg(0x01, 0x04); key_msg(0x01, 0x05); key_msg(0x01, 0x06);
    key_msg(0x02, 0x04);
    const hid_kbd_report_t *r = &ctx.kbd[ctx.kbd_count - 1];
    TEST_ASSERT_EQUAL(0x05, r->keys[0]);
    TEST_ASSERT_EQUAL(0x06, r->keys[1]);
    TEST_ASSERT_EQUAL(0x00, r->keys[2]);

    key_msg(0x01, 0xE1);
    TEST_ASSERT_EQUAL(0x02, ctx.kbd[ctx.kbd_count - 1].modifiers);
    key_msg(0x01, 0xE0); key_msg(0x01, 0xE7);
    TEST_ASSERT_EQUAL(0x83, ctx.kbd[ctx.kbd_count - 1].modifiers);
    key_msg(0x02, 0xE1);
    TEST_ASSERT_EQUAL(0x81, ctx.kbd[ctx.kbd_count - 1].modifiers);
    key_msg(0x02, 0xE0); key_msg(0x02, 0xE7);
    TEST_ASSERT_EQUAL(0x00, ctx.kbd[ctx.kbd_count - 1].modifiers);
}

void test_relative_failure_keeps_every_axis(void) {
    uint8_t m[] = {0x11, 0x00, 0x0A, 0x00, 0xEC, 0xFF, 0x03}; // dx=10 dy=-20 wheel=3
    ctx.fail_rel_next = 1;
    send_msg(m, 7);
    TEST_ASSERT_EQUAL(0, ctx.rel_count);
    hid_logic_tick(&logic, now + 1);
    TEST_ASSERT_EQUAL(1, ctx.rel_count);
    TEST_ASSERT_EQUAL(10, ctx.rel[0].dx);
    TEST_ASSERT_EQUAL(-20, ctx.rel[0].dy);
    TEST_ASSERT_EQUAL(3, ctx.rel[0].wheel);
}

static long drain_rel(int axis) { /* 0=dx 1=dy 2=wheel; sums what the following ticks send */
    long total = 0;
    size_t prev = ctx.rel_count;
    for (int t = 0; t < 200; t++) {
        hid_logic_tick(&logic, now);
        for (size_t i = prev; i < ctx.rel_count; i++) {
            total += axis == 0 ? ctx.rel[i].dx : axis == 1 ? ctx.rel[i].dy : ctx.rel[i].wheel;
        }
        if (ctx.rel_count == prev) break;
        prev = ctx.rel_count;
    }
    return total;
}

void test_relative_dy_and_wheel_saturate_and_wheel_min_is_minus_127(void) {
    ctx.is_ready = false;
    for (int i = 0; i < 3; i++) {
        uint8_t m[] = {0x11, 0x00, 0x00, 0x00, 0x20, 0x4E, 0x00}; // dy=20000
        now += 1000;
        send_msg(m, 7);
    }
    ctx.is_ready = true;
    TEST_ASSERT_EQUAL(32767, drain_rel(1));

    ctx.is_ready = false;
    for (int i = 0; i < 2; i++) {
        uint8_t m[] = {0x11, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80}; // wheel=-128 -> -127
        now += 1000;
        send_msg(m, 7);
    }
    ctx.is_ready = true;
    TEST_ASSERT_EQUAL(-254, drain_rel(2));
}

void test_absolute_x_clamp_and_edges(void) {
    uint8_t hi[] = {0x10, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00}; // x=32768
    send_msg(hi, 7);
    TEST_ASSERT_EQUAL(32767, ctx.abs[ctx.abs_count - 1].x);
    TEST_ASSERT_EQUAL(0, ctx.abs[ctx.abs_count - 1].y);
    uint8_t max[] = {0x10, 0x00, 0xFF, 0xFF, 0xFF, 0x7F, 0x00}; // x=65535 y=32767
    send_msg(max, 7);
    TEST_ASSERT_EQUAL(32767, ctx.abs[ctx.abs_count - 1].x);
    TEST_ASSERT_EQUAL(32767, ctx.abs[ctx.abs_count - 1].y);
    uint8_t ok[] = {0x10, 0x00, 0xFF, 0x7F, 0x01, 0x00, 0x00}; // x=32767 y=1
    send_msg(ok, 7);
    TEST_ASSERT_EQUAL(32767, ctx.abs[ctx.abs_count - 1].x);
    TEST_ASSERT_EQUAL(1, ctx.abs[ctx.abs_count - 1].y);
}

void test_pointer_switch_waits_until_the_sink_is_ready(void) {
    uint8_t mr[] = {0x11, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00};
    send_msg(mr, 7);
    TEST_ASSERT_EQUAL(1, ctx.rel_count);
    ctx.is_ready = false;
    uint8_t ma[] = {0x10, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00};
    send_msg(ma, 7);
    TEST_ASSERT_EQUAL(1, ctx.rel_count); // nothing may be pushed to a sink that is not ready
    TEST_ASSERT_EQUAL(0, ctx.abs_count);
    ctx.is_ready = true;
    hid_logic_tick(&logic, now);
    TEST_ASSERT_EQUAL(2, ctx.rel_count);
    TEST_ASSERT_EQUAL(0, ctx.rel[1].buttons);
    TEST_ASSERT_EQUAL(1, ctx.abs_count);
    TEST_ASSERT_EQUAL(1, ctx.abs[0].buttons);
}

void test_absolute_failure_is_retried(void) {
    uint8_t ma[] = {0x10, 0x01, 0x34, 0x12, 0x78, 0x56, 0x00};
    ctx.fail_abs_on = 1;
    send_msg(ma, 7);
    TEST_ASSERT_EQUAL(0, ctx.abs_count);
    ctx.fail_abs_on = 0;
    hid_logic_tick(&logic, now + 1);
    TEST_ASSERT_EQUAL(1, ctx.abs_count);
    TEST_ASSERT_EQUAL(1, ctx.abs[0].buttons);
    TEST_ASSERT_EQUAL(0x1234, ctx.abs[0].x);
    TEST_ASSERT_EQUAL(0x5678, ctx.abs[0].y);
}

void test_hold_timeout_covers_modifiers_buttons_and_keepalive(void) {
    /* modifier only */
    now = 10000; key_msg(0x01, 0xE1);
    hid_logic_tick(&logic, now + 3000);
    TEST_ASSERT_EQUAL(0x02, ctx.kbd[ctx.kbd_count - 1].modifiers); /* exactly the timeout: still held */
    hid_logic_tick(&logic, now + 3001);
    TEST_ASSERT_EQUAL(0x00, ctx.kbd[ctx.kbd_count - 1].modifiers);

    /* button only (absolute) */
    now = 20000;
    uint8_t ma[] = {0x10, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00};
    send_msg(ma, 7);
    size_t n = ctx.abs_count;
    hid_logic_tick(&logic, now + 3001);
    TEST_ASSERT_EQUAL(n + 1, ctx.abs_count);
    TEST_ASSERT_EQUAL(0, ctx.abs[ctx.abs_count - 1].buttons);

    /* KEEPALIVE refreshes the timeout */
    now = 40000; key_msg(0x01, 0x04);
    now = 42000;
    uint8_t ka[] = {0x04};
    TEST_ASSERT_EQUAL(HID_RESULT_OK, send_msg(ka, 1));
    hid_logic_tick(&logic, 44000); /* 4 s after the key, 2 s after the keepalive */
    TEST_ASSERT_EQUAL(0x04, ctx.kbd[ctx.kbd_count - 1].keys[0]);
    hid_logic_tick(&logic, 45001);
    TEST_ASSERT_EQUAL(0x00, ctx.kbd[ctx.kbd_count - 1].keys[0]);
}

void test_clock_going_backwards_does_not_release_held_keys(void) {
    now = 1000000; key_msg(0x01, 0x04);
    hid_logic_tick(&logic, 500); /* the clock jumped back */
    TEST_ASSERT_EQUAL(0x04, ctx.kbd[ctx.kbd_count - 1].keys[0]);
    hid_logic_tick(&logic, 500 + 3001);
    TEST_ASSERT_EQUAL(0x00, ctx.kbd[ctx.kbd_count - 1].keys[0]);
}

void test_absolute_button_change_bypasses_the_rate_limit(void) {
    uint8_t ka[] = {0x04};
    for (int i = 0; i < 100; i++) send_msg(ka, 1); /* empty the bucket */
    uint8_t press[] = {0x10, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00};
    TEST_ASSERT_EQUAL(HID_RESULT_OK, send_msg(press, 7));
    TEST_ASSERT_EQUAL(1, ctx.abs[ctx.abs_count - 1].buttons);
    TEST_ASSERT_EQUAL(HID_RESULT_RATE_LIMITED, send_msg(press, 7)); /* same buttons: limited */
}

void test_release_owner_only_for_the_owner_and_releases_everything(void) {
    key_msg(0x01, 0x04);
    size_t n = ctx.kbd_count;
    hid_logic_release_owner(&logic, 2); /* not the owner: nothing happens */
    TEST_ASSERT_EQUAL(n, ctx.kbd_count);
    TEST_ASSERT_FALSE(hid_logic_acquire(&logic, 2));
    hid_logic_release_owner(&logic, 1);
    TEST_ASSERT_EQUAL(n + 1, ctx.kbd_count);
    TEST_ASSERT_EQUAL(0x00, ctx.kbd[ctx.kbd_count - 1].keys[0]);
    TEST_ASSERT_TRUE(hid_logic_acquire(&logic, 2));
}

void test_usb_mounted_drops_a_pending_pointer_switch(void) {
    uint8_t mr[] = {0x11, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00};
    send_msg(mr, 7);
    ctx.fail_rel_next = 1; /* the release of the old (relative) interface fails: the switch stays pending */
    uint8_t ma[] = {0x10, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00};
    send_msg(ma, 7);
    TEST_ASSERT_EQUAL(1, ctx.rel_count);
    hid_logic_on_usb_mounted(&logic); /* the host forgot everything: no release report for the old side is needed */
    hid_logic_tick(&logic, now + 1);
    TEST_ASSERT_EQUAL(1, ctx.rel_count);
    TEST_ASSERT_EQUAL(0, ctx.abs[ctx.abs_count - 1].buttons);
}

void test_usb_mounted_discards_unsent_relative_movement(void) {
    uint8_t mr[] = {0x11, 0x00, 0x0A, 0x00, 0x05, 0x00, 0x01}; // dx=10 dy=5 wheel=1, never sent
    ctx.is_ready = false;
    send_msg(mr, 7);
    ctx.is_ready = true;
    hid_logic_on_usb_mounted(&logic);
    hid_logic_tick(&logic, now + 1);
    for (size_t i = 0; i < ctx.rel_count; i++) {
        TEST_ASSERT_EQUAL(0, ctx.rel[i].dx);
        TEST_ASSERT_EQUAL(0, ctx.rel[i].dy);
        TEST_ASSERT_EQUAL(0, ctx.rel[i].wheel);
    }
    TEST_ASSERT_TRUE(ctx.rel_count >= 1); // the all-released report itself still goes out
}

void test_click_never_overtakes_unsent_movement(void) {
    /* The sink is stuck: movement is queued, then a click arrives. The movement must go out under the OLD button
     * state, the press afterwards (otherwise the click lands before the cursor arrives). */
    uint8_t move[] = {0x11, 0x00, 0x0A, 0x00, 0x00, 0x00, 0x00};
    uint8_t click[] = {0x11, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00};
    ctx.is_ready = false;
    send_msg(move, 7);
    send_msg(click, 7);
    TEST_ASSERT_EQUAL(0, ctx.rel_count);
    ctx.is_ready = true;
    hid_logic_tick(&logic, now);
    TEST_ASSERT_EQUAL(2, ctx.rel_count);
    TEST_ASSERT_EQUAL(10, ctx.rel[0].dx);
    TEST_ASSERT_EQUAL(0, ctx.rel[0].buttons);
    TEST_ASSERT_EQUAL(0, ctx.rel[1].dx);
    TEST_ASSERT_EQUAL(1, ctx.rel[1].buttons);
}

void test_button_change_flushes_the_backlog_first(void) {
    uint8_t move[] = {0x11, 0x00, 0x0A, 0x00, 0x00, 0x00, 0x00};
    uint8_t press[] = {0x11, 0x01, 0x05, 0x00, 0x00, 0x00, 0x00}; /* press + 5 counts */
    ctx.fail_rel_next = 1; /* the movement report is refused once and stays queued */
    send_msg(move, 7);
    TEST_ASSERT_EQUAL(0, ctx.rel_count);
    send_msg(press, 7); /* the sink is fine now: backlog first under the old buttons, then the press with its own movement */
    TEST_ASSERT_EQUAL(2, ctx.rel_count);
    TEST_ASSERT_EQUAL(10, ctx.rel[0].dx);
    TEST_ASSERT_EQUAL(0, ctx.rel[0].buttons);
    TEST_ASSERT_EQUAL(5, ctx.rel[1].dx); /* not merged into the backlog */
    TEST_ASSERT_EQUAL(1, ctx.rel[1].buttons);
}

void test_movement_in_a_button_message_travels_with_the_new_buttons(void) {
    uint8_t drag[] = {0x11, 0x01, 0x05, 0x00, 0x00, 0x00, 0x00}; /* press + 5 counts, nothing queued before */
    send_msg(drag, 7);
    TEST_ASSERT_EQUAL(1, ctx.rel_count);
    TEST_ASSERT_EQUAL(5, ctx.rel[0].dx);
    TEST_ASSERT_EQUAL(1, ctx.rel[0].buttons);
}

void test_rate_bucket_refill_rate_and_cap(void) {
    uint8_t ka[] = {0x04};
    for (int i = 0; i < 100; i++) TEST_ASSERT_EQUAL(HID_RESULT_OK, send_msg(ka, 1));
    TEST_ASSERT_EQUAL(HID_RESULT_RATE_LIMITED, send_msg(ka, 1));
    now += 100; /* 400/s * 0.1 s = 40 tokens */
    for (int i = 0; i < 40; i++) TEST_ASSERT_EQUAL(HID_RESULT_OK, send_msg(ka, 1));
    TEST_ASSERT_EQUAL(HID_RESULT_RATE_LIMITED, send_msg(ka, 1));
    now += 60000; /* long idle: capped at the burst size */
    for (int i = 0; i < 100; i++) TEST_ASSERT_EQUAL(HID_RESULT_OK, send_msg(ka, 1));
    TEST_ASSERT_EQUAL(HID_RESULT_RATE_LIMITED, send_msg(ka, 1));
}

void test_abuse_window_resets(void) {
    uint8_t ka[] = {0x04};
    for (int i = 0; i < 100; i++) send_msg(ka, 1);
    for (int i = 0; i < 900; i++) TEST_ASSERT_EQUAL(HID_RESULT_RATE_LIMITED, send_msg(ka, 1));
    now += 5000; /* new window, the bucket is full again */
    for (int i = 0; i < 100; i++) TEST_ASSERT_EQUAL(HID_RESULT_OK, send_msg(ka, 1));
    for (int i = 0; i < 900; i++) TEST_ASSERT_EQUAL(HID_RESULT_RATE_LIMITED, send_msg(ka, 1)); /* 900 < 1000 in this window */
}

void test_release_all_clears_modifiers_buttons_and_both_pointers(void) {
    key_msg(0x01, 0xE0);
    uint8_t mr[] = {0x11, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00};
    send_msg(mr, 7);
    uint8_t ra[] = {0x03};
    size_t nk = ctx.kbd_count, nr = ctx.rel_count;
    TEST_ASSERT_EQUAL(HID_RESULT_OK, send_msg(ra, 1));
    TEST_ASSERT_EQUAL(nk + 1, ctx.kbd_count);
    TEST_ASSERT_EQUAL(0, ctx.kbd[ctx.kbd_count - 1].modifiers);
    TEST_ASSERT_EQUAL(nr + 1, ctx.rel_count);
    TEST_ASSERT_EQUAL(0, ctx.rel[ctx.rel_count - 1].buttons);

    uint8_t ma[] = {0x10, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00};
    send_msg(ma, 7);
    size_t na = ctx.abs_count;
    send_msg(ra, 1);
    TEST_ASSERT_EQUAL(na + 1, ctx.abs_count);
    TEST_ASSERT_EQUAL(0, ctx.abs[ctx.abs_count - 1].buttons);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_basic_keys_and_rollover);
    RUN_TEST(test_key_boundaries);
    RUN_TEST(test_absolute_pointer);
    RUN_TEST(test_relative_pointer_splits_and_saturation);
    RUN_TEST(test_relative_cap_per_call);
    RUN_TEST(test_relative_button_change_survives_sink_failure);
    RUN_TEST(test_rate_tokens_survive_a_very_long_idle);
    RUN_TEST(test_switch_to_abs_drops_stale_relative_movement);
    RUN_TEST(test_pointer_type_switch);
    RUN_TEST(test_pointer_type_switch_failed_retransmit);
    RUN_TEST(test_retransmit_and_ready_flag);
    RUN_TEST(test_rate_limiting_and_bypass);
    RUN_TEST(test_rate_limiting_abuse);
    RUN_TEST(test_rate_limiting_abuse_bypass_counts_as_abuse);
    RUN_TEST(test_owner_logic);
    RUN_TEST(test_hold_timeout);
    RUN_TEST(test_usb_mounted);
    RUN_TEST(test_time_reversing);
    RUN_TEST(test_key_release_keeps_order_and_modifier_bits);
    RUN_TEST(test_relative_failure_keeps_every_axis);
    RUN_TEST(test_relative_dy_and_wheel_saturate_and_wheel_min_is_minus_127);
    RUN_TEST(test_absolute_x_clamp_and_edges);
    RUN_TEST(test_pointer_switch_waits_until_the_sink_is_ready);
    RUN_TEST(test_absolute_failure_is_retried);
    RUN_TEST(test_hold_timeout_covers_modifiers_buttons_and_keepalive);
    RUN_TEST(test_clock_going_backwards_does_not_release_held_keys);
    RUN_TEST(test_absolute_button_change_bypasses_the_rate_limit);
    RUN_TEST(test_release_owner_only_for_the_owner_and_releases_everything);
    RUN_TEST(test_usb_mounted_drops_a_pending_pointer_switch);
    RUN_TEST(test_usb_mounted_discards_unsent_relative_movement);
    RUN_TEST(test_click_never_overtakes_unsent_movement);
    RUN_TEST(test_button_change_flushes_the_backlog_first);
    RUN_TEST(test_movement_in_a_button_message_travels_with_the_new_buttons);
    RUN_TEST(test_rate_bucket_refill_rate_and_cap);
    RUN_TEST(test_abuse_window_resets);
    RUN_TEST(test_release_all_clears_modifiers_buttons_and_both_pointers);
    return UNITY_END();
}
