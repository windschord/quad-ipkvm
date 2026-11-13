#include "unity.h"
#include "hid_logic.h"
#include "hid_msg.h"
#include <string.h>
#include <stdlib.h>

/* Fixed-seed fuzzing. Two kinds of input: raw random bytes (length 0..32) and well-formed messages of every type with
 * random values, so the state machine is really driven. The sink is flaky (fails, or is not ready) and checks the
 * invariants of every report that reaches it. Runs under ASan/UBSan (test/host/CMakeLists.txt). */

#define FUZZ_ITERATIONS 200000

typedef struct {
    bool ready;
    int fail_percent;
    size_t reports_this_call;
    size_t max_reports_per_call;
    bool last_kbd_valid;
    hid_kbd_report_t last_kbd;
    hid_abs_report_t last_abs;
    hid_rel_report_t last_rel;
} fuzz_ctx_t;

static fuzz_ctx_t fc;
static uint32_t seed = 42;

static uint32_t rnd(void) {
    seed = (seed * 1103515245u + 12345u) & 0x7fffffffu;
    return seed >> 8;
}

static bool flaky(void) { return (int)(rnd() % 100) < fc.fail_percent; }

static bool sink_ready(void *c) { (void)c; return fc.ready; }

static bool sink_kbd(void *c, const hid_kbd_report_t *r) {
    (void)c;
    if (flaky()) return false;
    fc.reports_this_call++;
    /* Either ErrorRollOver (all six slots 0x01) or distinct non-zero usages packed at the front. */
    bool all_rollover = true;
    for (int i = 0; i < 6; i++) if (r->keys[i] != 0x01) all_rollover = false;
    if (!all_rollover) {
        bool seen_zero = false;
        for (int i = 0; i < 6; i++) {
            if (r->keys[i] == 0) { seen_zero = true; continue; }
            TEST_ASSERT_FALSE_MESSAGE(seen_zero, "keys not packed at the front");
            TEST_ASSERT_TRUE_MESSAGE(r->keys[i] >= 0x04 && r->keys[i] <= 0xA4, "key usage out of range");
            for (int j = i + 1; j < 6; j++) {
                TEST_ASSERT_NOT_EQUAL_MESSAGE(r->keys[i], r->keys[j], "duplicate key in report");
            }
        }
    }
    fc.last_kbd = *r;
    fc.last_kbd_valid = true;
    return true;
}

static bool sink_abs(void *c, const hid_abs_report_t *r) {
    (void)c;
    if (flaky()) return false;
    fc.reports_this_call++;
    TEST_ASSERT_TRUE_MESSAGE(r->x <= HID_ABS_MAX && r->y <= HID_ABS_MAX, "abs coordinate out of range");
    TEST_ASSERT_TRUE_MESSAGE(r->buttons <= 7, "abs buttons out of range");
    TEST_ASSERT_TRUE_MESSAGE(r->wheel >= -127, "abs wheel -128");
    fc.last_abs = *r;
    return true;
}

static bool sink_rel(void *c, const hid_rel_report_t *r) {
    (void)c;
    if (flaky()) return false;
    fc.reports_this_call++;
    TEST_ASSERT_TRUE_MESSAGE(r->buttons <= 7, "rel buttons out of range");
    TEST_ASSERT_TRUE_MESSAGE(r->dx >= -127 && r->dy >= -127 && r->wheel >= -127, "rel value -128");
    fc.last_rel = *r;
    return true;
}

void setUp(void) {}
void tearDown(void) {}

static size_t random_valid_message(uint8_t *m) {
    switch (rnd() % 6) {
    case 0: m[0] = 0x01; m[1] = (uint8_t)(rnd() % 3 ? 0x04 + rnd() % 12 : rnd()); return 2; /* KEY_DOWN, mostly valid */
    case 1: m[0] = 0x02; m[1] = (uint8_t)(rnd() % 3 ? 0x04 + rnd() % 12 : rnd()); return 2; /* KEY_UP */
    case 2: m[0] = (rnd() % 8) ? 0x04 : 0x03; return 1;                                    /* KEEPALIVE / RELEASE_ALL */
    case 3: m[0] = 0x10; for (int i = 1; i < 7; i++) m[i] = (uint8_t)rnd(); return 7;       /* PTR_ABS */
    case 4: m[0] = 0x11; for (int i = 1; i < 7; i++) m[i] = (uint8_t)rnd(); return 7;       /* PTR_REL */
    default: m[0] = 0x01; m[1] = (uint8_t)(0xE0 + rnd() % 8); return 2;                    /* modifier down */
    }
}

static void test_fuzzing(void) {
    static const hid_sink_t sink = {
        .ctx = NULL, .ready = sink_ready, .send_keyboard = sink_kbd, .send_abs = sink_abs, .send_rel = sink_rel};
    hid_logic_t h;
    memset(&fc, 0, sizeof(fc));
    fc.ready = true;
    hid_logic_init(&h, NULL, &sink);
    TEST_ASSERT_TRUE(hid_logic_acquire(&h, 1));

    uint64_t now = 1000;
    for (int i = 0; i < FUZZ_ITERATIONS; i++) {
        now += rnd() % 40;
        if (rnd() % 5000 == 0) now -= now > 500 ? 500 : 0; /* occasionally the clock goes back */
        fc.ready = (rnd() % 10) != 0;
        fc.fail_percent = (rnd() % 4 == 0) ? 40 : 0;
        fc.reports_this_call = 0;

        uint8_t msg[32];
        size_t len;
        if (rnd() % 4 == 0) {
            len = rnd() % 33;
            for (size_t j = 0; j < len; j++) msg[j] = (uint8_t)rnd();
        } else {
            len = random_valid_message(msg);
        }
        hid_result_t res = hid_logic_on_message(&h, 1, msg, len, now);
        TEST_ASSERT_TRUE(res == HID_RESULT_OK || res == HID_RESULT_INVALID || res == HID_RESULT_RATE_LIMITED ||
                         res == HID_RESULT_ABUSE);
        TEST_ASSERT_EQUAL(HID_RESULT_NOT_OWNER, hid_logic_on_message(&h, 2, msg, len, now)); /* a stranger never gets in */

        /* One message may produce at most: 1 keyboard + 1 switch release + the capped relative reports + 1 abs. */
        TEST_ASSERT_TRUE_MESSAGE(fc.reports_this_call <= HID_REL_MAX_REPORTS_PER_CALL + 4, "too many reports per call");

        if (rnd() % 7 == 0) {
            fc.reports_this_call = 0;
            hid_logic_tick(&h, now);
            TEST_ASSERT_TRUE_MESSAGE(fc.reports_this_call <= HID_REL_MAX_REPORTS_PER_CALL + 4, "too many reports per tick");
        }

        /* State invariants */
        TEST_ASSERT_TRUE_MESSAGE(h.num_keys <= HID_MAX_KEYS, "num_keys overflow");
        for (size_t k = 0; k < h.num_keys; k++) {
            TEST_ASSERT_TRUE_MESSAGE(h.keys[k] >= 0x04 && h.keys[k] <= 0xA4, "invalid key in state");
            for (size_t l = k + 1; l < h.num_keys; l++) {
                TEST_ASSERT_NOT_EQUAL_MESSAGE(h.keys[k], h.keys[l], "duplicate keys in state");
            }
        }
        TEST_ASSERT_TRUE_MESSAGE(h.buttons <= 7, "buttons out of range");
        TEST_ASSERT_TRUE_MESSAGE(h.abs_x <= HID_ABS_MAX && h.abs_y <= HID_ABS_MAX, "abs out of bounds");
        TEST_ASSERT_TRUE_MESSAGE(h.rel_dx >= -32767 && h.rel_dx <= 32767, "rel_dx out of bounds");
        TEST_ASSERT_TRUE_MESSAGE(h.rel_dy >= -32767 && h.rel_dy <= 32767, "rel_dy out of bounds");
        TEST_ASSERT_TRUE_MESSAGE(h.rel_wheel >= -32767 && h.rel_wheel <= 32767, "rel_wheel out of bounds");

        /* RELEASE_ALL with a healthy sink leaves nothing pressed, anywhere. */
        if (len == 1 && msg[0] == 0x03 && res == HID_RESULT_OK && fc.ready) {
            TEST_ASSERT_EQUAL(0, h.modifiers);
            TEST_ASSERT_EQUAL(0, h.num_keys);
            TEST_ASSERT_EQUAL(0, h.buttons);
        }
    }

    /* Whatever happened, a healthy sink + a release_all + a few ticks ends with nothing pressed on the target. */
    fc.ready = true;
    fc.fail_percent = 0;
    hid_logic_release_all(&h);
    for (int t = 0; t < 200; t++) hid_logic_tick(&h, now + (uint64_t)t);
    TEST_ASSERT_TRUE(fc.last_kbd_valid);
    TEST_ASSERT_EQUAL(0, fc.last_kbd.modifiers);
    for (int i = 0; i < 6; i++) TEST_ASSERT_EQUAL(0, fc.last_kbd.keys[i]);
    TEST_ASSERT_EQUAL(0, fc.last_abs.buttons);
    TEST_ASSERT_EQUAL(0, fc.last_rel.buttons);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_fuzzing);
    return UNITY_END();
}
