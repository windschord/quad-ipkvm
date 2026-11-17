/* usb_vbus_ctrl: 起動時は切断、接続・切断それぞれのデバウンスの境界、途中の反転での数え直し、applied の失敗の再試行
 * （接続・切断の両方）、sample_valid = false（VBUS なし扱い）、enabled = false（いまの動作）、now_ms の逆行、
 * disconnect_debounce_ms の切り詰め、NULL、仕様の不変条件（VBUS が外れてから 10 秒未満）、
 * ファズ（独立な参照モデルとの比較と不変条件）。設計は docs/USB_VBUS_DESIGN.md、結果の表は docs/USB_VBUS_TESTS.md。 */
#include "unity.h"
#include "usb_vbus_ctrl.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define NONE USB_VBUS_CTRL_ACTION_NONE
#define CONNECT USB_VBUS_CTRL_ACTION_CONNECT
#define DISCONNECT USB_VBUS_CTRL_ACTION_DISCONNECT
#define FORCE_OFF USB_VBUS_CTRL_ACTION_FORCE_OFF

void setUp(void) {}
void tearDown(void) {}

static usb_vbus_ctrl_config_t cfg(uint32_t cd, uint32_t dd) {
    usb_vbus_ctrl_config_t c = usb_vbus_ctrl_config_default();
    c.enabled = true;
    c.connect_debounce_ms = cd;
    c.disconnect_debounce_ms = dd;
    return c;
}

static void init_cd_dd(usb_vbus_ctrl_t *c, uint32_t cd, uint32_t dd) {
    usb_vbus_ctrl_config_t k = cfg(cd, dd);
    usb_vbus_ctrl_init(c, &k);
}

/* 1 サンプル。present = VBUS あり、valid = 読み出し成功。 */
static usb_vbus_ctrl_action_t smp(usb_vbus_ctrl_t *c, uint64_t now, bool present, bool valid) {
    usb_vbus_ctrl_input_t in;
    in.now_ms = now;
    in.vbus_present = present;
    in.sample_valid = valid;
    return usb_vbus_ctrl_update(c, &in);
}
static usb_vbus_ctrl_action_t pres(usb_vbus_ctrl_t *c, uint64_t now) { return smp(c, now, true, true); }
static usb_vbus_ctrl_action_t abse(usb_vbus_ctrl_t *c, uint64_t now) { return smp(c, now, false, true); }

/* 接続の状態にする（t = 0 から VBUS あり、t = cd で CONNECT、applied ok）。戻り値は最後の now_ms（= cd）。 */
static uint64_t make_connected(usb_vbus_ctrl_t *c, uint32_t cd, uint32_t dd) {
    init_cd_dd(c, cd, dd);
    TEST_ASSERT_EQUAL_INT(cd == 0 ? CONNECT : NONE, pres(c, 0));
    if (cd != 0) TEST_ASSERT_EQUAL_INT(CONNECT, pres(c, cd));
    usb_vbus_ctrl_applied(c, CONNECT, true);
    TEST_ASSERT_TRUE(usb_vbus_ctrl_is_connected(c));
    return cd;
}

/* ---- 既定値と init ---- */
static void test_default_config(void) {
    usb_vbus_ctrl_config_t d = usb_vbus_ctrl_config_default();
    TEST_ASSERT_FALSE(d.enabled);
    TEST_ASSERT_EQUAL_UINT32(500, d.connect_debounce_ms);
    TEST_ASSERT_EQUAL_UINT32(1000, d.disconnect_debounce_ms);
    TEST_ASSERT_EQUAL_UINT32(1000, d.force_off_after_ms);
    TEST_ASSERT_EQUAL_UINT32(10000, USB_VBUS_CTRL_SPEC_REMOVE_LIMIT_MS);
    TEST_ASSERT_EQUAL_UINT32(3000, USB_VBUS_CTRL_DISCONNECT_DEBOUNCE_MAX_MS);
    TEST_ASSERT_EQUAL_UINT32(2000, USB_VBUS_CTRL_FORCE_OFF_MAX_MS);
    TEST_ASSERT_EQUAL_UINT32(1000, USB_VBUS_CTRL_POLL_PERIOD_MAX_MS);
    TEST_ASSERT_EQUAL_UINT32(2000, USB_VBUS_CTRL_COMPLETION_MARGIN_MS);
}

static void test_init_starts_disconnected_with_zero_stats_and_keeps_config(void) {
    usb_vbus_ctrl_t c;
    memset(&c, 0xA5, sizeof(c)); /* 汚れた領域 */
    usb_vbus_ctrl_config_t k = cfg(123, 456);
    usb_vbus_ctrl_init(&c, &k);
    TEST_ASSERT_FALSE(usb_vbus_ctrl_is_connected(&c));
    TEST_ASSERT_TRUE(c.config.enabled);
    TEST_ASSERT_EQUAL_UINT32(123, c.config.connect_debounce_ms);
    TEST_ASSERT_EQUAL_UINT32(456, c.config.disconnect_debounce_ms);
    usb_vbus_ctrl_stats_t s;
    memset(&s, 0xFF, sizeof(s));
    usb_vbus_ctrl_get_stats(&c, &s);
    TEST_ASSERT_EQUAL_UINT32(0, s.connects);
    TEST_ASSERT_EQUAL_UINT32(0, s.disconnects);
    TEST_ASSERT_EQUAL_UINT32(0, s.failures);
}

static void test_init_null_config_is_default(void) {
    usb_vbus_ctrl_t c;
    memset(&c, 0xA5, sizeof(c));
    usb_vbus_ctrl_init(&c, NULL);
    usb_vbus_ctrl_config_t d = usb_vbus_ctrl_config_default();
    TEST_ASSERT_FALSE(c.config.enabled);
    TEST_ASSERT_EQUAL_UINT32(d.connect_debounce_ms, c.config.connect_debounce_ms);
    TEST_ASSERT_EQUAL_UINT32(d.disconnect_debounce_ms, c.config.disconnect_debounce_ms);
    TEST_ASSERT_FALSE(usb_vbus_ctrl_is_connected(&c));
}

static void test_init_clamps_disconnect_debounce_to_max(void) {
    const uint32_t cases[][2] = {{0, 0}, {1, 1}, {2999, 2999}, {3000, 3000}, {3001, 3000}, {5000, 3000}, {10000, 3000}, {UINT32_MAX, 3000}};
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        usb_vbus_ctrl_t c;
        init_cd_dd(&c, 777, cases[i][0]);
        TEST_ASSERT_EQUAL_UINT32(cases[i][1], c.config.disconnect_debounce_ms);
        TEST_ASSERT_EQUAL_UINT32(777, c.config.connect_debounce_ms); /* 接続側は切り詰めない */
    }
    usb_vbus_ctrl_t c;
    init_cd_dd(&c, UINT32_MAX, 10);
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, c.config.connect_debounce_ms);
}

static void test_init_clamps_force_off_after_to_1_to_max(void) {
    const uint32_t cases[][2] = {{0, 1}, {1, 1}, {2, 2}, {1999, 1999}, {2000, 2000}, {2001, 2000}, {3000, 2000}, {UINT32_MAX, 2000}};
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        usb_vbus_ctrl_t c;
        usb_vbus_ctrl_config_t k = cfg(777, 888);
        k.force_off_after_ms = cases[i][0];
        usb_vbus_ctrl_init(&c, &k);
        TEST_ASSERT_EQUAL_UINT32(cases[i][1], c.config.force_off_after_ms);
        TEST_ASSERT_EQUAL_UINT32(888, c.config.disconnect_debounce_ms); /* 他の値は変わらない */
        TEST_ASSERT_EQUAL_UINT32(777, c.config.connect_debounce_ms);
    }
}

static void test_clamped_disconnect_debounce_is_what_the_state_machine_uses(void) {
    /* dd = 100000 を渡しても、3000 ms で切断される（仕様の 10 秒を超えない） */
    usb_vbus_ctrl_t c;
    uint64_t t = make_connected(&c, 500, 100000);
    TEST_ASSERT_EQUAL_INT(NONE, abse(&c, t + 100));
    TEST_ASSERT_EQUAL_INT(NONE, abse(&c, t + 100 + 2999));
    TEST_ASSERT_EQUAL_INT(DISCONNECT, abse(&c, t + 100 + 3000));
}

/* ---- 起動時は切断 ---- */
static void test_startup_is_disconnected_and_never_connects_without_vbus(void) {
    usb_vbus_ctrl_t c;
    init_cd_dd(&c, 500, 1000);
    TEST_ASSERT_FALSE(usb_vbus_ctrl_is_connected(&c));
    for (uint64_t t = 0; t <= 100000; t += 100) TEST_ASSERT_EQUAL_INT(NONE, abse(&c, t));
    TEST_ASSERT_FALSE(usb_vbus_ctrl_is_connected(&c));
}

static void test_startup_with_vbus_present_waits_for_debounce(void) {
    usb_vbus_ctrl_t c;
    init_cd_dd(&c, 500, 1000);
    TEST_ASSERT_EQUAL_INT(NONE, pres(&c, 0)); /* 最初の tick では接続しない */
    TEST_ASSERT_FALSE(usb_vbus_ctrl_is_connected(&c));
}

/* ---- 接続側のデバウンスの境界（-1 / ちょうど / +1） ---- */
static void test_connect_boundary_minus1_exact_plus1(void) {
    const uint32_t cds[] = {1, 2, 500, 1000, 12345};
    for (size_t i = 0; i < sizeof(cds) / sizeof(cds[0]); i++) {
        uint32_t cd = cds[i];
        usb_vbus_ctrl_t c;
        init_cd_dd(&c, cd, 1000);
        TEST_ASSERT_EQUAL_INT(NONE, pres(&c, 5000));
        TEST_ASSERT_EQUAL_INT(NONE, pres(&c, 5000 + cd - 1)); /* -1 */
        TEST_ASSERT_EQUAL_INT(CONNECT, pres(&c, 5000 + cd));  /* ちょうど */

        init_cd_dd(&c, cd, 1000);
        TEST_ASSERT_EQUAL_INT(NONE, pres(&c, 5000));
        TEST_ASSERT_EQUAL_INT(CONNECT, pres(&c, 5000 + cd + 1)); /* +1 */

        /* 同じことを、1 ms ごとの tick で（最初の CONNECT が経過ちょうどの tick） */
        init_cd_dd(&c, cd < 200 ? cd : 200, 1000);
        uint32_t want = cd < 200 ? cd : 200;
        for (uint32_t k = 0; k < want; k++) TEST_ASSERT_EQUAL_INT(NONE, pres(&c, 100 + k));
        TEST_ASSERT_EQUAL_INT(CONNECT, pres(&c, 100 + want));
    }
}

static void test_connect_debounce_zero_connects_on_first_detected_tick(void) {
    usb_vbus_ctrl_t c;
    init_cd_dd(&c, 0, 1000);
    TEST_ASSERT_EQUAL_INT(NONE, abse(&c, 0));
    TEST_ASSERT_EQUAL_INT(CONNECT, pres(&c, 1));
}

static void test_connect_run_restarts_after_a_flip(void) {
    usb_vbus_ctrl_t c;
    init_cd_dd(&c, 500, 1000);
    TEST_ASSERT_EQUAL_INT(NONE, pres(&c, 0));
    TEST_ASSERT_EQUAL_INT(NONE, pres(&c, 400));
    TEST_ASSERT_EQUAL_INT(NONE, abse(&c, 450));  /* 1 回の反転 */
    TEST_ASSERT_EQUAL_INT(NONE, pres(&c, 500));  /* 数え直しの始まり */
    TEST_ASSERT_EQUAL_INT(NONE, pres(&c, 999));  /* 経過 499 */
    TEST_ASSERT_EQUAL_INT(CONNECT, pres(&c, 1000)); /* 経過 500 */
}

static void test_connect_flip_one_ms_before_the_boundary_restarts(void) {
    usb_vbus_ctrl_t c;
    init_cd_dd(&c, 500, 1000);
    TEST_ASSERT_EQUAL_INT(NONE, pres(&c, 0));
    TEST_ASSERT_EQUAL_INT(NONE, abse(&c, 499));
    TEST_ASSERT_EQUAL_INT(NONE, pres(&c, 500)); /* 元の始まりからなら 500 だが、数え直し */
    TEST_ASSERT_EQUAL_INT(NONE, pres(&c, 999));
    TEST_ASSERT_EQUAL_INT(CONNECT, pres(&c, 1000));
}

/* ---- applied（接続） ---- */
static void test_applied_connect_ok_connects_and_counts(void) {
    usb_vbus_ctrl_t c;
    make_connected(&c, 500, 1000);
    usb_vbus_ctrl_stats_t s;
    usb_vbus_ctrl_get_stats(&c, &s);
    TEST_ASSERT_EQUAL_UINT32(1, s.connects);
    TEST_ASSERT_EQUAL_UINT32(0, s.disconnects);
    TEST_ASSERT_EQUAL_UINT32(0, s.failures);
    /* 接続の状態では、VBUS ありのサンプルは何も起こさない */
    for (uint64_t t = 600; t < 100000; t += 500) TEST_ASSERT_EQUAL_INT(NONE, pres(&c, t));
    TEST_ASSERT_TRUE(usb_vbus_ctrl_is_connected(&c));
}

static void test_connect_failure_retries_every_tick_without_redebounce(void) {
    usb_vbus_ctrl_t c;
    init_cd_dd(&c, 500, 1000);
    TEST_ASSERT_EQUAL_INT(NONE, pres(&c, 0));
    TEST_ASSERT_EQUAL_INT(CONNECT, pres(&c, 500));
    usb_vbus_ctrl_applied(&c, CONNECT, false);
    TEST_ASSERT_FALSE(usb_vbus_ctrl_is_connected(&c));
    TEST_ASSERT_EQUAL_INT(CONNECT, pres(&c, 501)); /* デバウンスをやり直さない */
    usb_vbus_ctrl_applied(&c, CONNECT, false);
    TEST_ASSERT_EQUAL_INT(CONNECT, pres(&c, 502));
    usb_vbus_ctrl_applied(&c, CONNECT, false);
    TEST_ASSERT_EQUAL_INT(CONNECT, pres(&c, 9000));
    TEST_ASSERT_FALSE(usb_vbus_ctrl_is_connected(&c));
    usb_vbus_ctrl_stats_t s;
    usb_vbus_ctrl_get_stats(&c, &s);
    TEST_ASSERT_EQUAL_UINT32(3, s.failures);
    TEST_ASSERT_EQUAL_UINT32(0, s.connects);
    usb_vbus_ctrl_applied(&c, CONNECT, true);
    TEST_ASSERT_TRUE(usb_vbus_ctrl_is_connected(&c));
    usb_vbus_ctrl_get_stats(&c, &s);
    TEST_ASSERT_EQUAL_UINT32(1, s.connects);
    TEST_ASSERT_EQUAL_UINT32(3, s.failures);
    TEST_ASSERT_EQUAL_INT(NONE, pres(&c, 9001));
}

static void test_connect_retry_stops_when_vbus_is_lost_and_debounce_restarts(void) {
    usb_vbus_ctrl_t c;
    init_cd_dd(&c, 500, 1000);
    TEST_ASSERT_EQUAL_INT(NONE, pres(&c, 0));
    TEST_ASSERT_EQUAL_INT(CONNECT, pres(&c, 500));
    usb_vbus_ctrl_applied(&c, CONNECT, false);
    TEST_ASSERT_EQUAL_INT(NONE, abse(&c, 600));  /* VBUS が消えた */
    TEST_ASSERT_EQUAL_INT(NONE, pres(&c, 700));  /* 戻った。ここから数え直し */
    TEST_ASSERT_EQUAL_INT(NONE, pres(&c, 1199));
    TEST_ASSERT_EQUAL_INT(CONNECT, pres(&c, 1200));
}

/* ---- 切断側のデバウンスの境界 ---- */
static void test_disconnect_boundary_minus1_exact_plus1(void) {
    const uint32_t dds[] = {1, 2, 1000, 2999, 3000};
    for (size_t i = 0; i < sizeof(dds) / sizeof(dds[0]); i++) {
        uint32_t dd = dds[i];
        usb_vbus_ctrl_t c;
        uint64_t t = make_connected(&c, 500, dd) + 10;
        TEST_ASSERT_EQUAL_INT(NONE, abse(&c, t));
        TEST_ASSERT_EQUAL_INT(NONE, abse(&c, t + dd - 1)); /* -1 */
        TEST_ASSERT_EQUAL_INT(DISCONNECT, abse(&c, t + dd)); /* ちょうど */

        t = make_connected(&c, 500, dd) + 10;
        TEST_ASSERT_EQUAL_INT(NONE, abse(&c, t));
        TEST_ASSERT_EQUAL_INT(DISCONNECT, abse(&c, t + dd + 1)); /* +1 */

        uint32_t want = dd < 200 ? dd : 200;
        make_connected(&c, 500, want);
        for (uint32_t k = 0; k < want; k++) TEST_ASSERT_EQUAL_INT(NONE, abse(&c, 1000 + k));
        TEST_ASSERT_EQUAL_INT(DISCONNECT, abse(&c, 1000 + want));
    }
}

static void test_disconnect_debounce_zero_disconnects_on_first_absent_tick(void) {
    usb_vbus_ctrl_t c;
    uint64_t t = make_connected(&c, 500, 0);
    TEST_ASSERT_EQUAL_INT(NONE, pres(&c, t + 1));
    TEST_ASSERT_EQUAL_INT(DISCONNECT, abse(&c, t + 2));
}

static void test_disconnect_run_restarts_after_a_flip(void) {
    usb_vbus_ctrl_t c;
    uint64_t t = make_connected(&c, 500, 1000); /* t = 500 */
    TEST_ASSERT_EQUAL_INT(NONE, abse(&c, t + 100));
    TEST_ASSERT_EQUAL_INT(NONE, abse(&c, t + 900));
    TEST_ASSERT_EQUAL_INT(NONE, pres(&c, t + 950)); /* 1 回だけ戻った */
    TEST_ASSERT_EQUAL_INT(NONE, abse(&c, t + 1000)); /* 数え直しの始まり */
    TEST_ASSERT_EQUAL_INT(NONE, abse(&c, t + 1999));
    TEST_ASSERT_EQUAL_INT(DISCONNECT, abse(&c, t + 2000));
}

static void test_disconnect_flip_one_ms_before_the_boundary_restarts(void) {
    usb_vbus_ctrl_t c;
    uint64_t t = make_connected(&c, 500, 1000);
    TEST_ASSERT_EQUAL_INT(NONE, abse(&c, t + 10));
    TEST_ASSERT_EQUAL_INT(NONE, pres(&c, t + 1009));
    TEST_ASSERT_EQUAL_INT(NONE, abse(&c, t + 1010));
    TEST_ASSERT_EQUAL_INT(NONE, abse(&c, t + 2009));
    TEST_ASSERT_EQUAL_INT(DISCONNECT, abse(&c, t + 2010));
}

/* ---- applied（切断） ---- */
static void test_applied_disconnect_ok_disconnects_and_counts(void) {
    usb_vbus_ctrl_t c;
    uint64_t t = make_connected(&c, 500, 1000);
    TEST_ASSERT_EQUAL_INT(NONE, abse(&c, t + 1));
    TEST_ASSERT_EQUAL_INT(DISCONNECT, abse(&c, t + 1001));
    usb_vbus_ctrl_applied(&c, DISCONNECT, true);
    TEST_ASSERT_FALSE(usb_vbus_ctrl_is_connected(&c));
    usb_vbus_ctrl_stats_t s;
    usb_vbus_ctrl_get_stats(&c, &s);
    TEST_ASSERT_EQUAL_UINT32(1, s.connects);
    TEST_ASSERT_EQUAL_UINT32(1, s.disconnects);
    TEST_ASSERT_EQUAL_UINT32(0, s.failures);
    /* 切断の状態では、VBUS なしのサンプルは何も起こさない */
    for (uint64_t k = 0; k < 50; k++) TEST_ASSERT_EQUAL_INT(NONE, abse(&c, t + 2000 + k * 1000));
}

static void test_disconnect_failure_retries_every_tick_without_redebounce(void) {
    usb_vbus_ctrl_t c;
    uint64_t t = make_connected(&c, 500, 1000);
    TEST_ASSERT_EQUAL_INT(NONE, abse(&c, t + 1));
    TEST_ASSERT_EQUAL_INT(DISCONNECT, abse(&c, t + 1001));
    usb_vbus_ctrl_applied(&c, DISCONNECT, false);
    TEST_ASSERT_TRUE(usb_vbus_ctrl_is_connected(&c)); /* 接続のまま */
    for (uint64_t k = 1; k <= 5; k++) {
        TEST_ASSERT_EQUAL_INT(DISCONNECT, abse(&c, t + 1001 + k)); /* 毎回 */
        usb_vbus_ctrl_applied(&c, DISCONNECT, false);
    }
    TEST_ASSERT_EQUAL_INT(DISCONNECT, abse(&c, t + 1001 + 999)); /* 最初の DISCONNECT から 999 < force_off_after_ms（既定 1000） */
    usb_vbus_ctrl_applied(&c, DISCONNECT, false);
    TEST_ASSERT_TRUE(usb_vbus_ctrl_is_connected(&c));
    usb_vbus_ctrl_stats_t s;
    usb_vbus_ctrl_get_stats(&c, &s);
    TEST_ASSERT_EQUAL_UINT32(7, s.failures);
    TEST_ASSERT_EQUAL_UINT32(7, s.disconnect_failure_count);
    TEST_ASSERT_EQUAL_UINT32(0, s.force_off_count);
    TEST_ASSERT_EQUAL_UINT32(0, s.disconnects);
    usb_vbus_ctrl_applied(&c, DISCONNECT, true);
    TEST_ASSERT_FALSE(usb_vbus_ctrl_is_connected(&c));
    usb_vbus_ctrl_get_stats(&c, &s);
    TEST_ASSERT_EQUAL_UINT32(1, s.disconnects);
    TEST_ASSERT_EQUAL_INT(NONE, abse(&c, t + 60001));
}

static void test_disconnect_retry_stops_when_vbus_returns_and_debounce_restarts(void) {
    usb_vbus_ctrl_t c;
    uint64_t t = make_connected(&c, 500, 1000);
    TEST_ASSERT_EQUAL_INT(NONE, abse(&c, t + 1));
    TEST_ASSERT_EQUAL_INT(DISCONNECT, abse(&c, t + 1001));
    usb_vbus_ctrl_applied(&c, DISCONNECT, false);
    TEST_ASSERT_EQUAL_INT(NONE, pres(&c, t + 1100)); /* VBUS が戻った: 切断しない */
    TEST_ASSERT_TRUE(usb_vbus_ctrl_is_connected(&c));
    TEST_ASSERT_EQUAL_INT(NONE, abse(&c, t + 1200));  /* また消えた。最初から数える */
    TEST_ASSERT_EQUAL_INT(NONE, abse(&c, t + 2199));
    TEST_ASSERT_EQUAL_INT(DISCONNECT, abse(&c, t + 2200));
}

/* ---- 状態が変わったら、数えを捨てる ---- */
static void test_after_connect_the_absent_run_is_counted_from_its_own_start(void) {
    /* 接続の tick までの「VBUS あり」の連続の始まりを、切断側の数えに持ち越さない */
    usb_vbus_ctrl_t c;
    init_cd_dd(&c, 500, 1000);
    TEST_ASSERT_EQUAL_INT(NONE, pres(&c, 0));
    TEST_ASSERT_EQUAL_INT(CONNECT, pres(&c, 500));
    usb_vbus_ctrl_applied(&c, CONNECT, true);
    TEST_ASSERT_EQUAL_INT(NONE, abse(&c, 600));   /* VBUS なしの始まり */
    TEST_ASSERT_EQUAL_INT(NONE, abse(&c, 1500));  /* 経過 900。0 から数えていれば 1500 で切断してしまう */
    TEST_ASSERT_EQUAL_INT(NONE, abse(&c, 1599));
    TEST_ASSERT_EQUAL_INT(DISCONNECT, abse(&c, 1600));
}

static void test_after_disconnect_the_present_run_is_counted_from_its_own_start(void) {
    usb_vbus_ctrl_t c;
    uint64_t t = make_connected(&c, 500, 1000);
    TEST_ASSERT_EQUAL_INT(NONE, abse(&c, t + 1));
    TEST_ASSERT_EQUAL_INT(DISCONNECT, abse(&c, t + 1001));
    usb_vbus_ctrl_applied(&c, DISCONNECT, true);
    TEST_ASSERT_EQUAL_INT(NONE, pres(&c, t + 1100)); /* VBUS ありの始まり */
    TEST_ASSERT_EQUAL_INT(NONE, pres(&c, t + 1599));
    TEST_ASSERT_EQUAL_INT(CONNECT, pres(&c, t + 1600));
}

static void test_failed_disconnect_is_counted_as_failure_not_as_disconnect(void) {
    /* 切断の失敗のあと、VBUS が戻って（接続のまま）、また外れたら、全体の経過ではなく新しい連続で数える */
    usb_vbus_ctrl_t c;
    uint64_t t = make_connected(&c, 500, 1000);
    TEST_ASSERT_EQUAL_INT(NONE, abse(&c, t + 1));
    TEST_ASSERT_EQUAL_INT(DISCONNECT, abse(&c, t + 1001));
    usb_vbus_ctrl_applied(&c, DISCONNECT, false);
    usb_vbus_ctrl_stats_t s;
    usb_vbus_ctrl_get_stats(&c, &s);
    TEST_ASSERT_EQUAL_UINT32(0, s.disconnects);
    TEST_ASSERT_EQUAL_UINT32(1, s.connects);
}

static void test_applied_none_is_ignored(void) {
    usb_vbus_ctrl_t c;
    init_cd_dd(&c, 500, 1000);
    usb_vbus_ctrl_applied(&c, NONE, true);
    usb_vbus_ctrl_applied(&c, NONE, false);
    TEST_ASSERT_FALSE(usb_vbus_ctrl_is_connected(&c));
    usb_vbus_ctrl_stats_t s;
    usb_vbus_ctrl_get_stats(&c, &s);
    TEST_ASSERT_EQUAL_UINT32(0, s.connects + s.disconnects + s.failures);
    /* NONE の ok = true / false は、接続の状態も変えない */
    make_connected(&c, 500, 1000);
    usb_vbus_ctrl_applied(&c, NONE, true);
    usb_vbus_ctrl_applied(&c, NONE, false);
    TEST_ASSERT_TRUE(usb_vbus_ctrl_is_connected(&c));
    usb_vbus_ctrl_get_stats(&c, &s);
    TEST_ASSERT_EQUAL_UINT32(1, s.connects);
    TEST_ASSERT_EQUAL_UINT32(0, s.failures);
    /* NONE は数えも消さない */
    TEST_ASSERT_EQUAL_INT(NONE, abse(&c, 600));
    usb_vbus_ctrl_applied(&c, NONE, true);
    TEST_ASSERT_EQUAL_INT(DISCONNECT, abse(&c, 1600));
}

/* ---- sample_valid = false（VBUS なし扱い） ---- */
static void test_invalid_sample_never_connects_even_if_vbus_present_is_true(void) {
    usb_vbus_ctrl_t c;
    init_cd_dd(&c, 500, 1000);
    for (uint64_t t = 0; t <= 100000; t += 50) TEST_ASSERT_EQUAL_INT(NONE, smp(&c, t, true, false));
    TEST_ASSERT_FALSE(usb_vbus_ctrl_is_connected(&c));
}

static void test_invalid_sample_breaks_a_connect_run(void) {
    usb_vbus_ctrl_t c;
    init_cd_dd(&c, 500, 1000);
    TEST_ASSERT_EQUAL_INT(NONE, pres(&c, 0));
    TEST_ASSERT_EQUAL_INT(NONE, pres(&c, 300));
    TEST_ASSERT_EQUAL_INT(NONE, smp(&c, 400, true, false)); /* 読み出しの失敗 */
    TEST_ASSERT_EQUAL_INT(NONE, pres(&c, 500));
    TEST_ASSERT_EQUAL_INT(NONE, pres(&c, 999));
    TEST_ASSERT_EQUAL_INT(CONNECT, pres(&c, 1000));
}

static void test_invalid_sample_counts_as_absent_when_connected(void) {
    usb_vbus_ctrl_t c;
    uint64_t t = make_connected(&c, 500, 1000);
    TEST_ASSERT_EQUAL_INT(NONE, smp(&c, t + 1, true, false));
    TEST_ASSERT_EQUAL_INT(NONE, smp(&c, t + 1000, true, false));
    TEST_ASSERT_EQUAL_INT(DISCONNECT, smp(&c, t + 1001, true, false));
}

static void test_invalid_and_absent_samples_form_one_continuous_run(void) {
    usb_vbus_ctrl_t c;
    uint64_t t = make_connected(&c, 500, 1000);
    TEST_ASSERT_EQUAL_INT(NONE, smp(&c, t + 1, false, false));
    TEST_ASSERT_EQUAL_INT(NONE, abse(&c, t + 400));
    TEST_ASSERT_EQUAL_INT(NONE, smp(&c, t + 800, true, false));
    TEST_ASSERT_EQUAL_INT(DISCONNECT, abse(&c, t + 1001));
}

static void test_invalid_sample_in_the_middle_of_a_disconnected_absent_run_is_harmless(void) {
    usb_vbus_ctrl_t c;
    init_cd_dd(&c, 500, 1000);
    for (uint64_t t = 0; t < 5000; t += 100) TEST_ASSERT_EQUAL_INT(NONE, smp(&c, t, (t / 100) % 2 == 0, false));
}

/* ---- enabled = false（いまの動作） ---- */
static void test_disabled_connects_once_on_first_update_then_none(void) {
    usb_vbus_ctrl_t c;
    usb_vbus_ctrl_config_t k = usb_vbus_ctrl_config_default(); /* enabled = false */
    usb_vbus_ctrl_init(&c, &k);
    TEST_ASSERT_EQUAL_INT(CONNECT, abse(&c, 0)); /* VBUS なしでも */
    usb_vbus_ctrl_applied(&c, CONNECT, true);
    TEST_ASSERT_TRUE(usb_vbus_ctrl_is_connected(&c));
    for (uint64_t t = 1; t < 100000; t += 997) {
        TEST_ASSERT_EQUAL_INT(NONE, abse(&c, t));
        TEST_ASSERT_EQUAL_INT(NONE, smp(&c, t + 1, true, false));
    }
    TEST_ASSERT_TRUE(usb_vbus_ctrl_is_connected(&c)); /* VBUS なしが続いても、切断しない */
}

static void test_disabled_ignores_input_and_clock(void) {
    usb_vbus_ctrl_t c;
    usb_vbus_ctrl_config_t k = usb_vbus_ctrl_config_default();
    usb_vbus_ctrl_init(&c, &k);
    TEST_ASSERT_EQUAL_INT(CONNECT, smp(&c, 123456789ull, false, false));
    TEST_ASSERT_EQUAL_INT(NONE, pres(&c, 5));      /* 時計が戻っても */
    TEST_ASSERT_EQUAL_INT(NONE, pres(&c, 100000)); /* VBUS ありが続いても */
}

static void test_disabled_with_null_input_still_connects_once(void) {
    usb_vbus_ctrl_t c;
    usb_vbus_ctrl_config_t k = usb_vbus_ctrl_config_default();
    usb_vbus_ctrl_init(&c, &k);
    TEST_ASSERT_EQUAL_INT(CONNECT, usb_vbus_ctrl_update(&c, NULL));
    TEST_ASSERT_EQUAL_INT(NONE, usb_vbus_ctrl_update(&c, NULL));
}

static void test_disabled_does_not_retry_after_failure(void) {
    usb_vbus_ctrl_t c;
    usb_vbus_ctrl_config_t k = usb_vbus_ctrl_config_default();
    usb_vbus_ctrl_init(&c, &k);
    TEST_ASSERT_EQUAL_INT(CONNECT, pres(&c, 0));
    usb_vbus_ctrl_applied(&c, CONNECT, false);
    TEST_ASSERT_FALSE(usb_vbus_ctrl_is_connected(&c));
    TEST_ASSERT_EQUAL_INT(NONE, pres(&c, 100000));
    usb_vbus_ctrl_stats_t s;
    usb_vbus_ctrl_get_stats(&c, &s);
    TEST_ASSERT_EQUAL_UINT32(1, s.failures);
}

static void test_disabled_ignores_debounce_values(void) {
    usb_vbus_ctrl_t c;
    usb_vbus_ctrl_config_t k = cfg(UINT32_MAX, UINT32_MAX);
    k.enabled = false;
    usb_vbus_ctrl_init(&c, &k);
    TEST_ASSERT_EQUAL_INT(CONNECT, pres(&c, 0));
}

/* ---- now_ms の逆行 ---- */
static void test_clock_backward_restarts_the_disconnect_count_from_the_new_now(void) {
    usb_vbus_ctrl_t c;
    make_connected(&c, 500, 1000); /* last = 500 */
    TEST_ASSERT_EQUAL_INT(NONE, abse(&c, 1000));
    TEST_ASSERT_EQUAL_INT(NONE, abse(&c, 1500));
    TEST_ASSERT_EQUAL_INT(NONE, abse(&c, 1200)); /* 逆行。ここから数え直す */
    TEST_ASSERT_EQUAL_INT(NONE, abse(&c, 2199)); /* 元の始まり（1000）からなら切断済みの経過 */
    TEST_ASSERT_EQUAL_INT(DISCONNECT, abse(&c, 2200));
}

static void test_clock_backward_by_one_ms_restarts(void) {
    usb_vbus_ctrl_t c;
    make_connected(&c, 500, 1000);
    TEST_ASSERT_EQUAL_INT(NONE, abse(&c, 1000));
    TEST_ASSERT_EQUAL_INT(NONE, abse(&c, 1999));
    TEST_ASSERT_EQUAL_INT(NONE, abse(&c, 1998)); /* 1 ms の逆行 */
    TEST_ASSERT_EQUAL_INT(NONE, abse(&c, 2997));
    TEST_ASSERT_EQUAL_INT(DISCONNECT, abse(&c, 2998));
}

static void test_clock_same_time_is_not_backward(void) {
    usb_vbus_ctrl_t c;
    make_connected(&c, 500, 1000);
    TEST_ASSERT_EQUAL_INT(NONE, abse(&c, 1000));
    TEST_ASSERT_EQUAL_INT(NONE, abse(&c, 1500));
    TEST_ASSERT_EQUAL_INT(NONE, abse(&c, 1500)); /* 同時刻 */
    TEST_ASSERT_EQUAL_INT(DISCONNECT, abse(&c, 2000)); /* 始まりは 1000 のまま */
}

static void test_clock_backward_restarts_the_connect_count(void) {
    usb_vbus_ctrl_t c;
    init_cd_dd(&c, 500, 1000);
    TEST_ASSERT_EQUAL_INT(NONE, pres(&c, 5000));
    TEST_ASSERT_EQUAL_INT(NONE, pres(&c, 5400));
    TEST_ASSERT_EQUAL_INT(NONE, pres(&c, 100)); /* 逆行（大きく） */
    TEST_ASSERT_EQUAL_INT(NONE, pres(&c, 599));
    TEST_ASSERT_EQUAL_INT(CONNECT, pres(&c, 600));
}

static void test_clock_backward_to_zero(void) {
    usb_vbus_ctrl_t c;
    make_connected(&c, 500, 1000);
    TEST_ASSERT_EQUAL_INT(NONE, abse(&c, 700));
    TEST_ASSERT_EQUAL_INT(NONE, abse(&c, 0));
    TEST_ASSERT_EQUAL_INT(NONE, abse(&c, 999));
    TEST_ASSERT_EQUAL_INT(DISCONNECT, abse(&c, 1000));
}

static void test_clock_backward_while_not_counting_has_no_effect(void) {
    usb_vbus_ctrl_t c;
    make_connected(&c, 500, 1000);
    TEST_ASSERT_EQUAL_INT(NONE, pres(&c, 9000));
    TEST_ASSERT_EQUAL_INT(NONE, pres(&c, 100)); /* 数えていない最中の逆行 */
    TEST_ASSERT_EQUAL_INT(NONE, abse(&c, 200));
    TEST_ASSERT_EQUAL_INT(NONE, abse(&c, 1199));
    TEST_ASSERT_EQUAL_INT(DISCONNECT, abse(&c, 1200));
}

static void test_repeated_backward_steps_delay_by_at_most_the_debounce_each(void) {
    usb_vbus_ctrl_t c;
    make_connected(&c, 500, 1000);
    uint64_t base = 100000;
    TEST_ASSERT_EQUAL_INT(NONE, abse(&c, base));
    for (int i = 0; i < 20; i++) { /* 毎回、少し戻る。数えは、そのつど、その時刻から */
        TEST_ASSERT_EQUAL_INT(NONE, abse(&c, base + 900));
        base -= 1;
        TEST_ASSERT_EQUAL_INT(NONE, abse(&c, base));
    }
    TEST_ASSERT_EQUAL_INT(DISCONNECT, abse(&c, base + 1000)); /* 時計が単調になれば、dd 後に切断 */
}

static void test_large_now_values_do_not_overflow(void) {
    usb_vbus_ctrl_t c;
    /* 接続: 経過が 2^32 ms を超える（32 ビットに切り詰めると、足りなくなる） */
    init_cd_dd(&c, UINT32_MAX, 1000);
    TEST_ASSERT_EQUAL_INT(NONE, pres(&c, 1));
    TEST_ASSERT_EQUAL_INT(NONE, pres(&c, (uint64_t)UINT32_MAX));          /* 経過 2^32 - 2 */
    TEST_ASSERT_EQUAL_INT(CONNECT, pres(&c, 1 + (uint64_t)UINT32_MAX));   /* 経過 UINT32_MAX ちょうど */

    init_cd_dd(&c, 1000, 1000);
    TEST_ASSERT_EQUAL_INT(NONE, pres(&c, 0));
    TEST_ASSERT_EQUAL_INT(CONNECT, pres(&c, (1ull << 32) + 5)); /* 経過 2^32 + 5。下位 32 ビットは 5 */

    init_cd_dd(&c, 1000, 1000);
    TEST_ASSERT_EQUAL_INT(NONE, pres(&c, 0));
    TEST_ASSERT_EQUAL_INT(CONNECT, pres(&c, (1ull << 33))); /* 下位 32 ビットは 0 */

    /* 時刻が 64 ビットの上限の近く */
    init_cd_dd(&c, 500, 1000);
    uint64_t top = UINT64_MAX - 5000;
    TEST_ASSERT_EQUAL_INT(NONE, pres(&c, top));
    TEST_ASSERT_EQUAL_INT(NONE, pres(&c, top + 499));
    TEST_ASSERT_EQUAL_INT(CONNECT, pres(&c, top + 500));
    usb_vbus_ctrl_applied(&c, CONNECT, true);
    TEST_ASSERT_EQUAL_INT(NONE, abse(&c, top + 600));
    TEST_ASSERT_EQUAL_INT(NONE, abse(&c, top + 600 + 999));
    TEST_ASSERT_EQUAL_INT(DISCONNECT, abse(&c, top + 600 + 1000));
}

static void test_first_tick_with_large_now_does_not_look_like_a_backward_clock(void) {
    usb_vbus_ctrl_t c;
    init_cd_dd(&c, 500, 1000);
    TEST_ASSERT_EQUAL_INT(NONE, pres(&c, 0)); /* 最初の now_ms が 0 */
    TEST_ASSERT_EQUAL_INT(CONNECT, pres(&c, 500));
    init_cd_dd(&c, 500, 1000);
    TEST_ASSERT_EQUAL_INT(NONE, pres(&c, 777));
    TEST_ASSERT_EQUAL_INT(CONNECT, pres(&c, 777 + 500));
}

/* ---- NULL ---- */
static void test_null_arguments_do_not_crash_and_are_safe(void) {
    usb_vbus_ctrl_t c;
    usb_vbus_ctrl_stats_t s;
    usb_vbus_ctrl_init(NULL, NULL);
    usb_vbus_ctrl_config_t k = cfg(1, 1);
    usb_vbus_ctrl_init(NULL, &k);
    usb_vbus_ctrl_input_t in = {0, true, true};
    TEST_ASSERT_EQUAL_INT(NONE, usb_vbus_ctrl_update(NULL, &in));
    TEST_ASSERT_EQUAL_INT(NONE, usb_vbus_ctrl_update(NULL, NULL));
    usb_vbus_ctrl_applied(NULL, CONNECT, true);
    usb_vbus_ctrl_applied(NULL, DISCONNECT, false);
    TEST_ASSERT_FALSE(usb_vbus_ctrl_is_connected(NULL));
    memset(&s, 0xFF, sizeof(s));
    usb_vbus_ctrl_get_stats(NULL, &s);
    TEST_ASSERT_EQUAL_UINT32(0, s.connects);
    TEST_ASSERT_EQUAL_UINT32(0, s.disconnects);
    TEST_ASSERT_EQUAL_UINT32(0, s.failures);
    init_cd_dd(&c, 1, 1);
    usb_vbus_ctrl_get_stats(&c, NULL);
}

static void test_null_input_is_an_invalid_sample_and_does_not_connect(void) {
    usb_vbus_ctrl_t c;
    init_cd_dd(&c, 500, 1000);
    TEST_ASSERT_EQUAL_INT(NONE, pres(&c, 0));
    TEST_ASSERT_EQUAL_INT(NONE, usb_vbus_ctrl_update(&c, NULL)); /* 連続を切る */
    TEST_ASSERT_EQUAL_INT(NONE, pres(&c, 500));
    TEST_ASSERT_EQUAL_INT(NONE, pres(&c, 999));
    TEST_ASSERT_EQUAL_INT(CONNECT, pres(&c, 1000));
    for (int i = 0; i < 3; i++) TEST_ASSERT_EQUAL_INT(NONE, usb_vbus_ctrl_update(&c, NULL)); /* init 直後の NULL も安全 */
}

static void test_null_input_counts_as_absent_when_connected(void) {
    usb_vbus_ctrl_t c;
    uint64_t t = make_connected(&c, 500, 1000);
    TEST_ASSERT_EQUAL_INT(NONE, abse(&c, t + 10));
    TEST_ASSERT_EQUAL_INT(NONE, usb_vbus_ctrl_update(&c, NULL)); /* 時計は進まないが、VBUS なしの連続は切れない */
    TEST_ASSERT_EQUAL_INT(DISCONNECT, abse(&c, t + 1010));
    usb_vbus_ctrl_applied(&c, DISCONNECT, false);
    TEST_ASSERT_EQUAL_INT(DISCONNECT, usb_vbus_ctrl_update(&c, NULL)); /* 再試行の最中も、NULL は VBUS なしの扱い */
    TEST_ASSERT_EQUAL_INT(DISCONNECT, abse(&c, t + 1020));
}

/* ---- 統計 ---- */
static void test_stats_count_connects_disconnects_failures(void) {
    usb_vbus_ctrl_t c;
    init_cd_dd(&c, 10, 10);
    uint64_t t = 0;
    for (int round = 0; round < 3; round++) {
        TEST_ASSERT_EQUAL_INT(NONE, pres(&c, t));
        t += 10;
        TEST_ASSERT_EQUAL_INT(CONNECT, pres(&c, t));
        usb_vbus_ctrl_applied(&c, CONNECT, false);
        TEST_ASSERT_EQUAL_INT(CONNECT, pres(&c, ++t));
        usb_vbus_ctrl_applied(&c, CONNECT, true);
        TEST_ASSERT_EQUAL_INT(NONE, abse(&c, ++t));
        t += 10;
        TEST_ASSERT_EQUAL_INT(DISCONNECT, abse(&c, t));
        usb_vbus_ctrl_applied(&c, DISCONNECT, false);
        usb_vbus_ctrl_applied(&c, DISCONNECT, false);
        TEST_ASSERT_EQUAL_INT(DISCONNECT, abse(&c, ++t));
        usb_vbus_ctrl_applied(&c, DISCONNECT, true);
        ++t;
    }
    usb_vbus_ctrl_stats_t s;
    usb_vbus_ctrl_get_stats(&c, &s);
    TEST_ASSERT_EQUAL_UINT32(3, s.connects);
    TEST_ASSERT_EQUAL_UINT32(3, s.disconnects);
    TEST_ASSERT_EQUAL_UINT32(9, s.failures);
}

/* ---- 強制遮断（FORCE_OFF）: 切断が成功しないとき ---- */
/* 接続の状態にして、t = 1000 から VBUS なし（連続の始まり）。デバウンス dd（>= 1）、強制遮断 fo。戻り値は 1000。 */
static uint64_t connected_absent_from_1000(usb_vbus_ctrl_t *c, uint32_t dd, uint32_t fo) {
    usb_vbus_ctrl_config_t k = cfg(500, dd);
    k.force_off_after_ms = fo;
    usb_vbus_ctrl_init(c, &k);
    TEST_ASSERT_EQUAL_INT(NONE, pres(c, 0));
    TEST_ASSERT_EQUAL_INT(CONNECT, pres(c, 500));
    usb_vbus_ctrl_applied(c, CONNECT, true);
    TEST_ASSERT_EQUAL_INT(NONE, pres(c, 900));
    TEST_ASSERT_EQUAL_INT(NONE, abse(c, 1000));
    return 1000;
}

static void test_force_off_boundary_minus1_exact_plus1(void) {
    const uint32_t fos[] = {1, 2, 1000, 1999, 2000};
    for (size_t i = 0; i < sizeof(fos) / sizeof(fos[0]); i++) {
        uint32_t fo = fos[i];
        usb_vbus_ctrl_t c;
        uint64_t t = connected_absent_from_1000(&c, 1000, fo);
        TEST_ASSERT_EQUAL_INT(DISCONNECT, abse(&c, t + 1000)); /* due */
        usb_vbus_ctrl_applied(&c, DISCONNECT, false);
        if (fo > 1) {
            TEST_ASSERT_EQUAL_INT(DISCONNECT, abse(&c, t + 1000 + fo - 1)); /* -1 */
            usb_vbus_ctrl_applied(&c, DISCONNECT, false);
        }
        TEST_ASSERT_EQUAL_INT(FORCE_OFF, abse(&c, t + 1000 + fo)); /* ちょうど */
        usb_vbus_ctrl_applied(&c, FORCE_OFF, false);
        TEST_ASSERT_EQUAL_INT(FORCE_OFF, abse(&c, t + 1000 + fo + 1)); /* 以後、毎回 */

        /* +1: 境界の 1 ms 後が、最初の FORCE_OFF の tick */
        t = connected_absent_from_1000(&c, 1000, fo);
        TEST_ASSERT_EQUAL_INT(DISCONNECT, abse(&c, t + 1000));
        usb_vbus_ctrl_applied(&c, DISCONNECT, false);
        TEST_ASSERT_EQUAL_INT(FORCE_OFF, abse(&c, t + 1000 + fo + 1));
    }
}

static void test_force_off_wait_starts_at_the_first_disconnect(void) {
    /* サンプルが疎で、最初の DISCONNECT が due より遅れたとき、待ちの起点は、最初の DISCONNECT を返した時刻 */
    usb_vbus_ctrl_t c;
    uint64_t t = connected_absent_from_1000(&c, 1000, 2000);
    TEST_ASSERT_EQUAL_INT(DISCONNECT, abse(&c, t + 1500)); /* due は t + 1000。最初の DISCONNECT は 500 ms 遅れ */
    usb_vbus_ctrl_applied(&c, DISCONNECT, false);
    TEST_ASSERT_EQUAL_INT(DISCONNECT, abse(&c, t + 1500 + 1999)); /* 最初の DISCONNECT から 1999 */
    usb_vbus_ctrl_applied(&c, DISCONNECT, false);
    TEST_ASSERT_EQUAL_INT(FORCE_OFF, abse(&c, t + 1500 + 2000)); /* ちょうど 2000 */
}

static void test_force_off_needs_a_disconnect_first_even_with_sparse_samples(void) {
    usb_vbus_ctrl_t c;
    uint64_t t = connected_absent_from_1000(&c, 1000, 2000);
    TEST_ASSERT_EQUAL_INT(DISCONNECT, abse(&c, t + 20000)); /* 最初の tick が、すでに dd + fo を大きく超えている。それでも DISCONNECT */
    usb_vbus_ctrl_applied(&c, DISCONNECT, false);
    TEST_ASSERT_EQUAL_INT(DISCONNECT, abse(&c, t + 20001)); /* 最初の DISCONNECT から 1 ms: まだ待ち */
    usb_vbus_ctrl_applied(&c, DISCONNECT, false);
    TEST_ASSERT_EQUAL_INT(FORCE_OFF, abse(&c, t + 22000));
}

static void test_force_off_ok_disconnects_and_counts_separately(void) {
    usb_vbus_ctrl_t c;
    uint64_t t = connected_absent_from_1000(&c, 1000, 2000);
    TEST_ASSERT_EQUAL_INT(DISCONNECT, abse(&c, t + 1000));
    usb_vbus_ctrl_applied(&c, DISCONNECT, false);
    TEST_ASSERT_EQUAL_INT(FORCE_OFF, abse(&c, t + 3000));
    usb_vbus_ctrl_applied(&c, FORCE_OFF, true);
    TEST_ASSERT_FALSE(usb_vbus_ctrl_is_connected(&c));
    usb_vbus_ctrl_stats_t s;
    usb_vbus_ctrl_get_stats(&c, &s);
    TEST_ASSERT_EQUAL_UINT32(1, s.connects);
    TEST_ASSERT_EQUAL_UINT32(0, s.disconnects);
    TEST_ASSERT_EQUAL_UINT32(1, s.force_off_count);
    TEST_ASSERT_EQUAL_UINT32(1, s.disconnect_failure_count);
    TEST_ASSERT_EQUAL_UINT32(1, s.failures);
    for (uint64_t k = 0; k < 20; k++) TEST_ASSERT_EQUAL_INT(NONE, abse(&c, t + 3001 + k * 1000)); /* 切断の状態: 何も起きない */
    /* 切断のあとの再接続: 新しい VBUS ありの連続 */
    TEST_ASSERT_EQUAL_INT(NONE, pres(&c, t + 30000));
    TEST_ASSERT_EQUAL_INT(CONNECT, pres(&c, t + 30500));
}

static void test_force_off_failure_retries_every_tick_and_is_counted_as_a_failure(void) {
    usb_vbus_ctrl_t c;
    uint64_t t = connected_absent_from_1000(&c, 1000, 2000);
    TEST_ASSERT_EQUAL_INT(DISCONNECT, abse(&c, t + 1000));
    usb_vbus_ctrl_applied(&c, DISCONNECT, false);
    for (uint64_t k = 0; k < 5; k++) {
        TEST_ASSERT_EQUAL_INT(FORCE_OFF, abse(&c, t + 3000 + k)); /* DISCONNECT に戻らない */
        usb_vbus_ctrl_applied(&c, FORCE_OFF, false);
        TEST_ASSERT_TRUE(usb_vbus_ctrl_is_connected(&c));
    }
    TEST_ASSERT_EQUAL_INT(FORCE_OFF, abse(&c, t + 100000));
    usb_vbus_ctrl_stats_t s;
    usb_vbus_ctrl_get_stats(&c, &s);
    TEST_ASSERT_EQUAL_UINT32(6, s.failures);                  /* DISCONNECT 1 + FORCE_OFF 5 */
    TEST_ASSERT_EQUAL_UINT32(1, s.disconnect_failure_count);  /* FORCE_OFF の失敗は数えない */
    TEST_ASSERT_EQUAL_UINT32(0, s.force_off_count);
    usb_vbus_ctrl_applied(&c, FORCE_OFF, true);
    TEST_ASSERT_FALSE(usb_vbus_ctrl_is_connected(&c));
    usb_vbus_ctrl_get_stats(&c, &s);
    TEST_ASSERT_EQUAL_UINT32(1, s.force_off_count);
}

static void test_disconnect_success_ends_the_force_off_wait(void) {
    usb_vbus_ctrl_t c;
    uint64_t t = connected_absent_from_1000(&c, 1000, 2000);
    TEST_ASSERT_EQUAL_INT(DISCONNECT, abse(&c, t + 1000));
    usb_vbus_ctrl_applied(&c, DISCONNECT, false);
    TEST_ASSERT_EQUAL_INT(DISCONNECT, abse(&c, t + 2000));
    usb_vbus_ctrl_applied(&c, DISCONNECT, true); /* 成功 */
    TEST_ASSERT_FALSE(usb_vbus_ctrl_is_connected(&c));
    TEST_ASSERT_EQUAL_INT(NONE, abse(&c, t + 3000)); /* FORCE_OFF は来ない */
    TEST_ASSERT_EQUAL_INT(NONE, abse(&c, t + 60000));
    usb_vbus_ctrl_stats_t s;
    usb_vbus_ctrl_get_stats(&c, &s);
    TEST_ASSERT_EQUAL_UINT32(0, s.force_off_count);
    TEST_ASSERT_EQUAL_UINT32(1, s.disconnects);
    /* もう一度、接続して、外れる: 待ちは最初から（前回の記録を持ち越さない） */
    TEST_ASSERT_EQUAL_INT(NONE, pres(&c, t + 61000));
    TEST_ASSERT_EQUAL_INT(CONNECT, pres(&c, t + 61500));
    usb_vbus_ctrl_applied(&c, CONNECT, true);
    TEST_ASSERT_EQUAL_INT(NONE, abse(&c, t + 62000));
    TEST_ASSERT_EQUAL_INT(NONE, abse(&c, t + 62999));
    /* 最初は DISCONNECT（前回の「試した」と起点を持ち越さない。持ち越すと、ここで FORCE_OFF になる） */
    TEST_ASSERT_EQUAL_INT(DISCONNECT, abse(&c, t + 65000));
    usb_vbus_ctrl_applied(&c, DISCONNECT, false);
    TEST_ASSERT_EQUAL_INT(DISCONNECT, abse(&c, t + 66999));
    usb_vbus_ctrl_applied(&c, DISCONNECT, false);
    TEST_ASSERT_EQUAL_INT(FORCE_OFF, abse(&c, t + 67000));
}

static void test_vbus_return_restarts_the_force_off_wait(void) {
    usb_vbus_ctrl_t c;
    uint64_t t = connected_absent_from_1000(&c, 1000, 2000);
    TEST_ASSERT_EQUAL_INT(DISCONNECT, abse(&c, t + 1000));
    usb_vbus_ctrl_applied(&c, DISCONNECT, false);
    TEST_ASSERT_EQUAL_INT(DISCONNECT, abse(&c, t + 2900));
    usb_vbus_ctrl_applied(&c, DISCONNECT, false);
    TEST_ASSERT_EQUAL_INT(NONE, pres(&c, t + 2950)); /* VBUS が戻った */
    TEST_ASSERT_EQUAL_INT(NONE, abse(&c, t + 3000)); /* また外れた: 連続の始まり */
    TEST_ASSERT_EQUAL_INT(NONE, abse(&c, t + 3999));
    TEST_ASSERT_EQUAL_INT(DISCONNECT, abse(&c, t + 4000)); /* デバウンスのあとの DISCONNECT（FORCE_OFF ではない） */
    usb_vbus_ctrl_applied(&c, DISCONNECT, false);
    TEST_ASSERT_EQUAL_INT(DISCONNECT, abse(&c, t + 5999));
    usb_vbus_ctrl_applied(&c, DISCONNECT, false);
    TEST_ASSERT_EQUAL_INT(FORCE_OFF, abse(&c, t + 6000));
}

static void test_vbus_return_during_force_off_retry_cancels_it(void) {
    usb_vbus_ctrl_t c;
    uint64_t t = connected_absent_from_1000(&c, 1000, 2000);
    TEST_ASSERT_EQUAL_INT(DISCONNECT, abse(&c, t + 1000));
    usb_vbus_ctrl_applied(&c, DISCONNECT, false);
    TEST_ASSERT_EQUAL_INT(FORCE_OFF, abse(&c, t + 3000));
    usb_vbus_ctrl_applied(&c, FORCE_OFF, false);
    TEST_ASSERT_EQUAL_INT(NONE, pres(&c, t + 3100)); /* VBUS が戻った: 強制遮断しない */
    TEST_ASSERT_TRUE(usb_vbus_ctrl_is_connected(&c));
    TEST_ASSERT_EQUAL_INT(NONE, smp(&c, t + 3200, true, false)); /* 読み出しの失敗は VBUS なし: 連続の始まり */
    TEST_ASSERT_EQUAL_INT(DISCONNECT, smp(&c, t + 4200, true, false)); /* DISCONNECT から（FORCE_OFF ではない） */
}

static void test_invalid_samples_count_as_absent_for_force_off(void) {
    usb_vbus_ctrl_t c;
    uint64_t t = connected_absent_from_1000(&c, 1000, 2000);
    TEST_ASSERT_EQUAL_INT(DISCONNECT, smp(&c, t + 1000, true, false));
    usb_vbus_ctrl_applied(&c, DISCONNECT, false);
    TEST_ASSERT_EQUAL_INT(FORCE_OFF, smp(&c, t + 3000, true, false));
}

static void test_clock_backward_restarts_the_force_off_wait(void) {
    usb_vbus_ctrl_t c;
    uint64_t t = connected_absent_from_1000(&c, 1000, 2000);
    TEST_ASSERT_EQUAL_INT(DISCONNECT, abse(&c, t + 1200)); /* 試した */
    usb_vbus_ctrl_applied(&c, DISCONNECT, false);
    TEST_ASSERT_EQUAL_INT(NONE, abse(&c, t + 500)); /* 逆行: 数え直し（経過 0） */
    TEST_ASSERT_EQUAL_INT(DISCONNECT, abse(&c, t + 500 + 3000)); /* 試した記録（t + 1200）は捨てたので、まず DISCONNECT */
    usb_vbus_ctrl_applied(&c, DISCONNECT, false);
    TEST_ASSERT_EQUAL_INT(DISCONNECT, abse(&c, t + 500 + 3000 + 1999));
    usb_vbus_ctrl_applied(&c, DISCONNECT, false);
    TEST_ASSERT_EQUAL_INT(FORCE_OFF, abse(&c, t + 500 + 3000 + 2000));
}

static void test_force_off_after_zero_is_one_ms(void) {
    usb_vbus_ctrl_t c;
    uint64_t t = connected_absent_from_1000(&c, 1000, 0); /* init が 1 に直す */
    TEST_ASSERT_EQUAL_UINT32(1, c.config.force_off_after_ms);
    TEST_ASSERT_EQUAL_INT(DISCONNECT, abse(&c, t + 1000)); /* 最初は DISCONNECT（直接 FORCE_OFF ではない） */
    usb_vbus_ctrl_applied(&c, DISCONNECT, false);
    TEST_ASSERT_EQUAL_INT(FORCE_OFF, abse(&c, t + 1001));
}

static void test_force_off_with_zero_disconnect_debounce(void) {
    usb_vbus_ctrl_t c;
    usb_vbus_ctrl_config_t k = cfg(500, 0);
    k.force_off_after_ms = 100;
    usb_vbus_ctrl_init(&c, &k);
    TEST_ASSERT_EQUAL_INT(NONE, pres(&c, 0));
    TEST_ASSERT_EQUAL_INT(CONNECT, pres(&c, 500));
    usb_vbus_ctrl_applied(&c, CONNECT, true);
    TEST_ASSERT_EQUAL_INT(DISCONNECT, abse(&c, 600));
    usb_vbus_ctrl_applied(&c, DISCONNECT, false);
    TEST_ASSERT_EQUAL_INT(DISCONNECT, abse(&c, 699));
    usb_vbus_ctrl_applied(&c, DISCONNECT, false);
    TEST_ASSERT_EQUAL_INT(FORCE_OFF, abse(&c, 700));
}

static void test_force_off_is_never_returned_when_disabled_or_disconnected(void) {
    usb_vbus_ctrl_t c;
    usb_vbus_ctrl_config_t k = usb_vbus_ctrl_config_default(); /* enabled = false */
    usb_vbus_ctrl_init(&c, &k);
    TEST_ASSERT_EQUAL_INT(CONNECT, abse(&c, 0));
    for (uint64_t t = 1; t < 100000; t += 313) TEST_ASSERT_EQUAL_INT(NONE, abse(&c, t));
    init_cd_dd(&c, 500, 1000);
    for (uint64_t t = 0; t < 100000; t += 313) TEST_ASSERT_EQUAL_INT(NONE, abse(&c, t)); /* 切断の状態 */
}

static void test_applied_force_off_with_null_is_safe(void) {
    usb_vbus_ctrl_applied(NULL, FORCE_OFF, true);
    usb_vbus_ctrl_applied(NULL, FORCE_OFF, false);
}

/* ---- 仕様の不変条件（USB 2.0 §7.2.1: VBUS が外れたら 10 秒以内） ---- */
/* 物理的な外れは、最後に VBUS ありを読んだ時刻 lp より後（最悪で lp の直後）。ここでは、lp から測る（最悪側）。 */
static void test_spec_removal_bound_for_fixed_sample_periods(void) {
    const uint32_t periods[] = {1, 7, 10, 100, 999, 1000};
    const uint32_t dds[] = {0, 1, 999, 1000, 2999, 3000, 3001, 100000, UINT32_MAX};
    const uint32_t fos[] = {0, 1, 1000, 1999, 2000, 2001, UINT32_MAX};
    const uint64_t limit = USB_VBUS_CTRL_SPEC_REMOVE_LIMIT_MS - USB_VBUS_CTRL_COMPLETION_MARGIN_MS; /* 8000 */
    for (size_t pi = 0; pi < sizeof(periods) / sizeof(periods[0]); pi++) {
        for (size_t di = 0; di < sizeof(dds) / sizeof(dds[0]); di++) {
            for (size_t fi = 0; fi < sizeof(fos) / sizeof(fos[0]); fi++) {
                for (uint32_t phase = 0; phase < 2; phase++) { /* 外れる時刻の、サンプルとの相対位置を変える */
                    uint32_t P = periods[pi];
                    usb_vbus_ctrl_t c;
                    usb_vbus_ctrl_config_t k = cfg(500, dds[di]);
                    k.force_off_after_ms = fos[fi];
                    usb_vbus_ctrl_init(&c, &k);
                    uint32_t dd = c.config.disconnect_debounce_ms, fo = c.config.force_off_after_ms;
                    TEST_ASSERT_TRUE(dd <= USB_VBUS_CTRL_DISCONNECT_DEBOUNCE_MAX_MS);
                    TEST_ASSERT_TRUE(fo >= 1 && fo <= USB_VBUS_CTRL_FORCE_OFF_MAX_MS);
                    uint64_t t = 0;
                    TEST_ASSERT_EQUAL_INT(NONE, pres(&c, t));
                    t += 500;
                    TEST_ASSERT_EQUAL_INT(CONNECT, pres(&c, t));
                    usb_vbus_ctrl_applied(&c, CONNECT, true);
                    for (int n = 0; n < 3; n++) {
                        t += P;
                        TEST_ASSERT_EQUAL_INT(NONE, pres(&c, t));
                    }
                    uint64_t lp = t;                 /* 最後の VBUS ありのサンプル */
                    uint64_t ta = lp + (phase ? 1 : P); /* 最初の VBUS なしのサンプル（間隔は P 以下） */
                    uint64_t now = ta;
                    usb_vbus_ctrl_action_t a = abse(&c, now);
                    uint64_t first = 0;
                    bool got_first = false;
                    while (a != FORCE_OFF) {
                        if (a == DISCONNECT) {
                            if (!got_first) {
                                got_first = true;
                                first = now;
                                /* 2. 最初の action は、lp から 2P + D 以内（ta から D + P 以内） */
                                TEST_ASSERT_TRUE(now - ta >= dd);
                                TEST_ASSERT_TRUE(now - ta <= (uint64_t)dd + P);
                                TEST_ASSERT_TRUE(now - lp <= 2ull * P + dd);
                                TEST_ASSERT_TRUE(now - lp <= limit);
                            }
                            usb_vbus_ctrl_applied(&c, DISCONNECT, false); /* 失敗が続く */
                        } else {
                            TEST_ASSERT_EQUAL_INT(NONE, a);
                            TEST_ASSERT_FALSE(got_first); /* 一度 DISCONNECT が出たら、NONE には戻らない */
                        }
                        now += P;
                        a = abse(&c, now);
                        TEST_ASSERT_TRUE(now - ta < 3u * USB_VBUS_CTRL_SPEC_REMOVE_LIMIT_MS); /* 止まらない */
                    }
                    TEST_ASSERT_TRUE(got_first);
                    /* 3. FORCE_OFF は、最初の action から F 以上 F + P 以内、lp から 3P + D + F 以内 */
                    TEST_ASSERT_TRUE(now - first >= fo);
                    TEST_ASSERT_TRUE(now - first <= (uint64_t)fo + P);
                    TEST_ASSERT_TRUE(now - lp <= 3ull * P + dd + fo);
                    TEST_ASSERT_TRUE(now - lp <= limit); /* P <= 1000 のとき、8000 ms 以下（= 10 秒 - 完了の余裕 2 秒） */
                }
            }
        }
    }
}

/* 上限の境界: P = 1000、D = 3000、F = 2000。間隔が P 以下で、最悪の位置に tick が来る。
 * lp = 0（最後の VBUS あり）、ta = 1000（最初の VBUS なし。物理的な外れは、lp の直後）。 */
static void test_spec_removal_bound_at_the_limits(void) {
    usb_vbus_ctrl_t c;
    usb_vbus_ctrl_config_t k = cfg(0, 3000);
    k.force_off_after_ms = 2000;
    usb_vbus_ctrl_init(&c, &k);
    TEST_ASSERT_EQUAL_UINT32(3000, c.config.disconnect_debounce_ms);
    TEST_ASSERT_EQUAL_UINT32(2000, c.config.force_off_after_ms);
    TEST_ASSERT_EQUAL_INT(CONNECT, pres(&c, 0));
    usb_vbus_ctrl_applied(&c, CONNECT, true);
    TEST_ASSERT_EQUAL_INT(NONE, abse(&c, 1000));  /* ta */
    TEST_ASSERT_EQUAL_INT(NONE, abse(&c, 2000));
    TEST_ASSERT_EQUAL_INT(NONE, abse(&c, 3000));
    TEST_ASSERT_EQUAL_INT(NONE, abse(&c, 3999));  /* 経過 2999 */
    TEST_ASSERT_EQUAL_INT(DISCONNECT, abse(&c, 4999)); /* 経過 3999。lp から 4999 <= 2P + D = 5000 */
    usb_vbus_ctrl_applied(&c, DISCONNECT, false);
    TEST_ASSERT_EQUAL_INT(DISCONNECT, abse(&c, 5999));
    usb_vbus_ctrl_applied(&c, DISCONNECT, false);
    TEST_ASSERT_EQUAL_INT(DISCONNECT, abse(&c, 6998)); /* 最初の DISCONNECT から 1999 */
    usb_vbus_ctrl_applied(&c, DISCONNECT, false);
    TEST_ASSERT_EQUAL_INT(FORCE_OFF, abse(&c, 7998));  /* lp から 7998 <= 3P + D + F = 8000 */
    TEST_ASSERT_TRUE(7998 <= USB_VBUS_CTRL_SPEC_REMOVE_LIMIT_MS - USB_VBUS_CTRL_COMPLETION_MARGIN_MS);
    TEST_ASSERT_EQUAL_UINT32(8000, 3 * USB_VBUS_CTRL_POLL_PERIOD_MAX_MS + USB_VBUS_CTRL_DISCONNECT_DEBOUNCE_MAX_MS + USB_VBUS_CTRL_FORCE_OFF_MAX_MS);
}

/* 切断が成功する場合は、最初の action（DISCONNECT）で終わる。 */
static void test_spec_removal_bound_when_disconnect_succeeds(void) {
    const uint32_t periods[] = {1, 10, 500, 1000};
    for (size_t pi = 0; pi < sizeof(periods) / sizeof(periods[0]); pi++) {
        uint32_t P = periods[pi];
        usb_vbus_ctrl_t c;
        init_cd_dd(&c, 500, 100000); /* 3000 に切り詰め */
        TEST_ASSERT_EQUAL_INT(NONE, pres(&c, 0));
        TEST_ASSERT_EQUAL_INT(CONNECT, pres(&c, 500));
        usb_vbus_ctrl_applied(&c, CONNECT, true);
        uint64_t ta = 500 + P, now = ta;
        usb_vbus_ctrl_action_t a = abse(&c, now);
        while (a == NONE) {
            now += P;
            a = abse(&c, now);
        }
        TEST_ASSERT_EQUAL_INT(DISCONNECT, a);
        TEST_ASSERT_TRUE(now - ta <= 3000u + P);
        TEST_ASSERT_TRUE(now - 500 <= 2u * P + 3000u);
        TEST_ASSERT_TRUE(now - ta < USB_VBUS_CTRL_SPEC_REMOVE_LIMIT_MS);
        usb_vbus_ctrl_applied(&c, DISCONNECT, true);
        TEST_ASSERT_EQUAL_INT(NONE, abse(&c, now + P));
    }
}

/* ======== ファズ ======== */
typedef struct {
    uint64_t s;
} rng_t;
static uint64_t rnd(rng_t *r) { /* xorshift64* */
    r->s ^= r->s >> 12;
    r->s ^= r->s << 25;
    r->s ^= r->s >> 27;
    return r->s * 2685821657736338717ull;
}
static uint32_t rn(rng_t *r, uint32_t n) { return (uint32_t)(rnd(r) % n); }

#define FAIL_FMT(...)                                  \
    do {                                               \
        char _m[256];                                  \
        snprintf(_m, sizeof(_m), __VA_ARGS__);         \
        TEST_FAIL_MESSAGE(_m);                         \
    } while (0)

/* 独立に書いた参照モデル: 実装の内部状態（run_start、disc_first など）を持たず、サンプルの履歴から「いまの連続の始まり」を
 * 後ろ向きに探す。「最初の DISCONNECT を返した時刻」も、履歴の中の返した action から求める。 */
#define MAXH 1024
typedef struct {
    uint64_t t[MAXH];
    bool detected[MAXH];
    usb_vbus_ctrl_action_t act[MAXH];
    size_t n;
    size_t floor_idx; /* 状態が変わった（applied ok）あとの、最初のサンプルの添字 */
    bool connected;
    uint32_t cd, dd, fo;
    uint32_t connects, disconnects, failures, force_offs, disc_failures;
} model_t;

static void model_init(model_t *m, uint32_t cd, uint32_t dd, uint32_t fo) {
    memset(m, 0, sizeof(*m));
    m->cd = cd;
    m->dd = dd > 3000u ? 3000u : dd;
    m->fo = fo == 0 ? 1u : (fo > 2000u ? 2000u : fo);
}
static usb_vbus_ctrl_action_t model_step(model_t *m, uint64_t now, bool detected) {
    size_t i = m->n++;
    m->t[i] = now;
    m->detected[i] = detected;
    m->act[i] = NONE;
    bool want = !m->connected; /* 数えるサンプルの種類（切断の状態なら「検出」、接続の状態なら「検出なし」） */
    if (detected != want) return NONE;
    size_t j = i;
    while (j > m->floor_idx && m->detected[j - 1] == want && m->t[j] >= m->t[j - 1]) j--;
    uint64_t elapsed = now - m->t[j];
    usb_vbus_ctrl_action_t a = NONE;
    if (m->connected) {
        if (elapsed >= m->dd) {
            bool tried = false;
            uint64_t first = 0;
            for (size_t k = j; k < i && !tried; k++) {
                if (m->act[k] == DISCONNECT) {
                    tried = true;
                    first = m->t[k];
                }
            }
            a = (tried && now - first >= m->fo) ? FORCE_OFF : DISCONNECT;
        }
    } else if (elapsed >= m->cd) {
        a = CONNECT;
    }
    m->act[i] = a;
    return a;
}
static void model_applied(model_t *m, usb_vbus_ctrl_action_t a, bool ok) {
    if (a == NONE) return;
    if (!ok) {
        m->failures++;
        if (a == DISCONNECT) m->disc_failures++;
        return;
    }
    m->connected = (a == CONNECT);
    m->floor_idx = m->n;
    if (a == CONNECT) m->connects++;
    else if (a == DISCONNECT) m->disconnects++;
    else m->force_offs++;
}

static const uint32_t FZ_CD[] = {0, 1, 50, 500, 2000};
static const uint32_t FZ_DD[] = {0, 1, 100, 1000, 2999, 3000, 7000};
static const uint32_t FZ_FO[] = {0, 1, 100, 1000, 2000, 4000};
static const uint32_t FZ_P[] = {1, 10, 100, 500, 1000};
static const uint32_t FZ_STAY[] = {50, 90, 99}; /* 同じ VBUS を保つ確率（%） */
static const uint32_t FZ_INVALID[] = {0, 5, 30};
static const uint32_t FZ_CFAIL[] = {0, 30, 60};       /* applied(CONNECT) の失敗の確率（%） */
static const uint32_t FZ_DFAIL[] = {0, 50, 95, 100};  /* applied(DISCONNECT) */
static const uint32_t FZ_FFAIL[] = {0, 50, 90};       /* applied(FORCE_OFF) */

#define FZ_SEEDS 800
#define FZ_TICKS 400

static bool fz_ok(rng_t *r, usb_vbus_ctrl_action_t a, uint32_t cf, uint32_t df, uint32_t ff) {
    uint32_t pf = a == CONNECT ? cf : (a == DISCONNECT ? df : ff);
    return rn(r, 100) >= pf;
}

static void test_fuzz_monotone_against_reference_model_and_invariants(void) {
    uint64_t total_connect = 0, total_disconnect = 0, total_force_off = 0, total_conn_retry = 0, total_disc_retry = 0,
             total_fo_retry = 0, total_first_checks = 0, total_fo_checks = 0;
    for (uint32_t seed = 1; seed <= FZ_SEEDS; seed++) {
        rng_t r = {0x9E3779B97F4A7C15ull ^ ((uint64_t)seed * 0xD1B54A32D192ED03ull)};
        uint32_t cd = FZ_CD[rn(&r, 5)], dd = FZ_DD[rn(&r, 7)], fo = FZ_FO[rn(&r, 6)], P = FZ_P[rn(&r, 5)];
        uint32_t stay = FZ_STAY[rn(&r, 3)], inv = FZ_INVALID[rn(&r, 3)];
        uint32_t cf = FZ_CFAIL[rn(&r, 3)], df = FZ_DFAIL[rn(&r, 4)], ff = FZ_FFAIL[rn(&r, 3)];
        bool never_present = (seed % 10) == 0; /* VBUS が一度も present にならないシード */
        usb_vbus_ctrl_t c;
        usb_vbus_ctrl_config_t k = cfg(cd, dd);
        k.force_off_after_ms = fo;
        usb_vbus_ctrl_init(&c, &k);
        model_t m;
        model_init(&m, cd, dd, fo);
        uint32_t dd_eff = c.config.disconnect_debounce_ms, fo_eff = c.config.force_off_after_ms;
        if (dd_eff != m.dd || fo_eff != m.fo) FAIL_FMT("seed %u: clamp mismatch dd %u/%u fo %u/%u", seed, dd_eff, m.dd, fo_eff, m.fo);

        bool tconn = false; /* テスト側が追う「接続」の状態（applied ok だけで変わる） */
        bool vbus = false;
        uint64_t now = 1000 + rn(&r, 100000);
        bool pres_active = false, abs_active = false;
        uint64_t pres_start = 0, abs_start = 0;
        bool disc_seen = false, act_seen = false, fo_seen = false;
        uint64_t act_time = 0, lp = 0;
        bool have_lp = false;
        usb_vbus_ctrl_action_t prev_failed = NONE;

        for (uint32_t kt = 0; kt < FZ_TICKS; kt++) {
            if (kt > 0) now += 1 + rn(&r, P); /* 間隔は 1..P ms（単調） */
            if (rn(&r, 100) >= stay) vbus = !vbus;
            bool valid = rn(&r, 100) >= inv;
            bool present = never_present ? false : vbus;
            bool detected = valid && present;

            /* 連続の始まり（テスト側。実装とモデルを使わない） */
            if (detected) {
                if (!pres_active) { pres_active = true; pres_start = now; }
                lp = now;
                have_lp = true;
                abs_active = false;
                disc_seen = act_seen = fo_seen = false;
            } else {
                if (!abs_active) { abs_active = true; abs_start = now; }
                pres_active = false;
            }

            usb_vbus_ctrl_action_t a = smp(&c, now, present, valid);
            usb_vbus_ctrl_action_t want = model_step(&m, now, detected);
            if (a != want) FAIL_FMT("seed %u tick %u: action %d != model %d", seed, kt, (int)a, (int)want);

            /* 不変条件 */
            if (never_present && a == CONNECT) FAIL_FMT("seed %u tick %u: CONNECT without VBUS ever", seed, kt);
            if (a == CONNECT) {
                if (tconn) FAIL_FMT("seed %u tick %u: CONNECT while connected", seed, kt);
                if (!pres_active || now - pres_start < cd) FAIL_FMT("seed %u tick %u: CONNECT without a continuous valid VBUS run", seed, kt);
            }
            if (!tconn && pres_active && now - pres_start >= cd && a != CONNECT) FAIL_FMT("seed %u tick %u: no CONNECT though VBUS held >= cd", seed, kt);
            if (a == DISCONNECT || a == FORCE_OFF) {
                if (!tconn) FAIL_FMT("seed %u tick %u: %d while disconnected", seed, kt, (int)a);
                if (!abs_active || now - abs_start < dd_eff) FAIL_FMT("seed %u tick %u: %d before dd", seed, kt, (int)a);
                if (!act_seen) { /* 最初の action: 外れてから dd + P 以内（1） */
                    if (now - abs_start > (uint64_t)dd_eff + P) FAIL_FMT("seed %u tick %u: first action too late (%llu)", seed, kt, (unsigned long long)(now - abs_start));
                    if (!have_lp) FAIL_FMT("seed %u tick %u: action without ever seeing VBUS", seed, kt);
                    /* 物理的な外れ（最悪で lp の直後）から、2P + D 以内。8000 ms（10 秒 - 完了の余裕）以下 */
                    if (now - lp > 2ull * P + dd_eff) FAIL_FMT("seed %u tick %u: first action %llu ms after the last VBUS sample (> 2P + D)", seed, kt, (unsigned long long)(now - lp));
                    if (now - lp > USB_VBUS_CTRL_SPEC_REMOVE_LIMIT_MS - USB_VBUS_CTRL_COMPLETION_MARGIN_MS) FAIL_FMT("seed %u tick %u: first action beyond 8000 ms", seed, kt);
                    act_seen = true;
                    act_time = now;
                    total_first_checks++;
                }
            }
            if (a == DISCONNECT) {
                if (fo_seen) FAIL_FMT("seed %u tick %u: DISCONNECT after FORCE_OFF in the same absence", seed, kt);
                disc_seen = true;
            }
            if (a == FORCE_OFF) {
                if (!disc_seen) FAIL_FMT("seed %u tick %u: FORCE_OFF without a DISCONNECT before", seed, kt);
                if (now - act_time < fo_eff) FAIL_FMT("seed %u tick %u: FORCE_OFF before fo after the first DISCONNECT", seed, kt);
                if (!fo_seen) { /* 最初の FORCE_OFF: 最初の action から fo + P 以内、外れから 3P + D + F 以内（8000 ms 以下） */
                    if (now - act_time > (uint64_t)fo_eff + P) FAIL_FMT("seed %u tick %u: FORCE_OFF %llu ms after the first action", seed, kt, (unsigned long long)(now - act_time));
                    if (now - lp > 3ull * P + dd_eff + fo_eff) FAIL_FMT("seed %u tick %u: FORCE_OFF too late (%llu > 3P + D + F)", seed, kt, (unsigned long long)(now - lp));
                    if (now - lp > USB_VBUS_CTRL_SPEC_REMOVE_LIMIT_MS - USB_VBUS_CTRL_COMPLETION_MARGIN_MS) FAIL_FMT("seed %u tick %u: FORCE_OFF beyond 8000 ms", seed, kt);
                    fo_seen = true;
                    total_fo_checks++;
                }
            }
            if (tconn && abs_active && now - abs_start >= dd_eff && a != DISCONNECT && a != FORCE_OFF) FAIL_FMT("seed %u tick %u: no action though VBUS absent >= dd", seed, kt);
            if (tconn && abs_active && disc_seen && a != FORCE_OFF && a != DISCONNECT) FAIL_FMT("seed %u tick %u: action stopped", seed, kt);
            if (tconn && abs_active && disc_seen && now - act_time >= fo_eff && a != FORCE_OFF) FAIL_FMT("seed %u tick %u: no FORCE_OFF though DISCONNECT failed for >= fo", seed, kt);
            if (a == DISCONNECT && prev_failed == DISCONNECT) total_disc_retry++;
            if (a == CONNECT && prev_failed == CONNECT) total_conn_retry++;
            if (a == FORCE_OFF && prev_failed == FORCE_OFF) total_fo_retry++;
            prev_failed = NONE;

            if (a != NONE) {
                bool ok = fz_ok(&r, a, cf, df, ff);
                usb_vbus_ctrl_applied(&c, a, ok);
                model_applied(&m, a, ok);
                if (ok) {
                    tconn = (a == CONNECT);
                    abs_active = false; /* 状態が変わったら、新しい連続として数え直すのは、次のサンプルから */
                    pres_active = false;
                    disc_seen = act_seen = fo_seen = false;
                    if (a == CONNECT) total_connect++;
                    else if (a == DISCONNECT) total_disconnect++;
                    else total_force_off++;
                } else {
                    prev_failed = a;
                }
            }
            if (usb_vbus_ctrl_is_connected(&c) != tconn || m.connected != tconn) FAIL_FMT("seed %u tick %u: connected state mismatch", seed, kt);
        }
        usb_vbus_ctrl_stats_t s;
        usb_vbus_ctrl_get_stats(&c, &s);
        if (s.connects != m.connects || s.disconnects != m.disconnects || s.failures != m.failures || s.force_off_count != m.force_offs ||
            s.disconnect_failure_count != m.disc_failures)
            FAIL_FMT("seed %u: stats %u/%u/%u/%u/%u != model %u/%u/%u/%u/%u", seed, s.connects, s.disconnects, s.failures, s.force_off_count,
                     s.disconnect_failure_count, m.connects, m.disconnects, m.failures, m.force_offs, m.disc_failures);
        if (s.connects - s.disconnects - s.force_off_count != (tconn ? 1u : 0u)) FAIL_FMT("seed %u: counters inconsistent with state", seed);
        if (s.disconnect_failure_count > s.failures) FAIL_FMT("seed %u: disconnect failures exceed failures", seed);
    }
    /* 空振りで通っていないこと */
    TEST_ASSERT_TRUE_MESSAGE(total_connect > 2000, "fuzz made too few connects");
    TEST_ASSERT_TRUE_MESSAGE(total_disconnect > 800, "fuzz made too few disconnects");
    TEST_ASSERT_TRUE_MESSAGE(total_force_off > 150, "fuzz made too few force-offs");
    TEST_ASSERT_TRUE_MESSAGE(total_conn_retry > 200, "fuzz made too few connect retries");
    TEST_ASSERT_TRUE_MESSAGE(total_disc_retry > 200, "fuzz made too few disconnect retries");
    TEST_ASSERT_TRUE_MESSAGE(total_fo_retry > 50, "fuzz made too few force-off retries");
    TEST_ASSERT_TRUE_MESSAGE(total_first_checks > 1500, "fuzz checked the first-action bound too few times");
    TEST_ASSERT_TRUE_MESSAGE(total_fo_checks > 300, "fuzz checked the force-off bound too few times");
}

/* now_ms が戻る・同時刻が混じる系列。モデルとの一致だけを見る（単調でないので、仕様の時間の不変条件は対象外）。 */
static void test_fuzz_non_monotone_clock_against_reference_model(void) {
    uint64_t total_back = 0, total_actions = 0, total_fo = 0;
    for (uint32_t seed = 1; seed <= FZ_SEEDS; seed++) {
        rng_t r = {0xC2B2AE3D27D4EB4Full ^ ((uint64_t)seed * 0x9E3779B97F4A7C15ull)};
        uint32_t cd = FZ_CD[rn(&r, 5)], dd = FZ_DD[rn(&r, 7)], fo = FZ_FO[rn(&r, 6)], P = FZ_P[rn(&r, 5)];
        uint32_t stay = FZ_STAY[rn(&r, 3)], inv = FZ_INVALID[rn(&r, 3)];
        uint32_t cf = FZ_CFAIL[rn(&r, 3)], df = FZ_DFAIL[rn(&r, 4)], ff = FZ_FFAIL[rn(&r, 3)];
        usb_vbus_ctrl_t c;
        usb_vbus_ctrl_config_t k = cfg(cd, dd);
        k.force_off_after_ms = fo;
        usb_vbus_ctrl_init(&c, &k);
        model_t m;
        model_init(&m, cd, dd, fo);
        bool vbus = false;
        uint64_t now = 3000000;
        for (uint32_t kt = 0; kt < FZ_TICKS; kt++) {
            uint32_t kind = rn(&r, 10);
            if (kind == 0) { /* 逆行 */
                uint64_t back = 1 + rn(&r, 2 * P);
                now = now >= back ? now - back : 0;
                total_back++;
            } else if (kind == 1) { /* 同時刻 */
            } else {
                now += 1 + rn(&r, P);
            }
            if (rn(&r, 100) >= stay) vbus = !vbus;
            bool valid = rn(&r, 100) >= inv;
            usb_vbus_ctrl_action_t a = smp(&c, now, vbus, valid);
            usb_vbus_ctrl_action_t want = model_step(&m, now, valid && vbus);
            if (a != want) FAIL_FMT("seed %u tick %u: action %d != model %d (now %llu)", seed, kt, (int)a, (int)want, (unsigned long long)now);
            if (a != NONE) {
                bool ok = fz_ok(&r, a, cf, df, ff);
                usb_vbus_ctrl_applied(&c, a, ok);
                model_applied(&m, a, ok);
                total_actions++;
                if (a == FORCE_OFF) total_fo++;
            }
            if (usb_vbus_ctrl_is_connected(&c) != m.connected) FAIL_FMT("seed %u tick %u: connected mismatch", seed, kt);
        }
        usb_vbus_ctrl_stats_t s;
        usb_vbus_ctrl_get_stats(&c, &s);
        if (s.connects != m.connects || s.disconnects != m.disconnects || s.failures != m.failures || s.force_off_count != m.force_offs ||
            s.disconnect_failure_count != m.disc_failures)
            FAIL_FMT("seed %u: stats differ from the model", seed);
    }
    TEST_ASSERT_TRUE_MESSAGE(total_back > 5000, "fuzz made too few backward steps");
    TEST_ASSERT_TRUE_MESSAGE(total_actions > 3000, "fuzz made too few actions");
    TEST_ASSERT_TRUE_MESSAGE(total_fo > 100, "fuzz made too few force-offs");
}

/* 検出なし（enabled = false）: どんな入力でも、最初の update だけ CONNECT。 */
static void test_fuzz_disabled_connects_exactly_once(void) {
    for (uint32_t seed = 1; seed <= 200; seed++) {
        rng_t r = {0x1234567ull * seed + 99};
        usb_vbus_ctrl_t c;
        usb_vbus_ctrl_config_t k = cfg(rn(&r, 3000), rn(&r, 8000));
        k.enabled = false;
        usb_vbus_ctrl_init(&c, &k);
        uint32_t connects = 0;
        for (uint32_t i = 0; i < 100; i++) {
            usb_vbus_ctrl_action_t a = smp(&c, rnd(&r), rn(&r, 2), rn(&r, 2));
            if (a == CONNECT) {
                if (i != 0) FAIL_FMT("seed %u: CONNECT at tick %u", seed, i);
                connects++;
                usb_vbus_ctrl_applied(&c, CONNECT, rn(&r, 2));
            } else if (a != NONE) {
                FAIL_FMT("seed %u tick %u: unexpected action %d", seed, i, (int)a);
            }
        }
        TEST_ASSERT_EQUAL_UINT32(1, connects);
    }
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_default_config);
    RUN_TEST(test_init_starts_disconnected_with_zero_stats_and_keeps_config);
    RUN_TEST(test_init_null_config_is_default);
    RUN_TEST(test_init_clamps_disconnect_debounce_to_max);
    RUN_TEST(test_init_clamps_force_off_after_to_1_to_max);
    RUN_TEST(test_clamped_disconnect_debounce_is_what_the_state_machine_uses);
    RUN_TEST(test_startup_is_disconnected_and_never_connects_without_vbus);
    RUN_TEST(test_startup_with_vbus_present_waits_for_debounce);
    RUN_TEST(test_connect_boundary_minus1_exact_plus1);
    RUN_TEST(test_connect_debounce_zero_connects_on_first_detected_tick);
    RUN_TEST(test_connect_run_restarts_after_a_flip);
    RUN_TEST(test_connect_flip_one_ms_before_the_boundary_restarts);
    RUN_TEST(test_applied_connect_ok_connects_and_counts);
    RUN_TEST(test_connect_failure_retries_every_tick_without_redebounce);
    RUN_TEST(test_connect_retry_stops_when_vbus_is_lost_and_debounce_restarts);
    RUN_TEST(test_disconnect_boundary_minus1_exact_plus1);
    RUN_TEST(test_disconnect_debounce_zero_disconnects_on_first_absent_tick);
    RUN_TEST(test_disconnect_run_restarts_after_a_flip);
    RUN_TEST(test_disconnect_flip_one_ms_before_the_boundary_restarts);
    RUN_TEST(test_applied_disconnect_ok_disconnects_and_counts);
    RUN_TEST(test_disconnect_failure_retries_every_tick_without_redebounce);
    RUN_TEST(test_disconnect_retry_stops_when_vbus_returns_and_debounce_restarts);
    RUN_TEST(test_after_connect_the_absent_run_is_counted_from_its_own_start);
    RUN_TEST(test_after_disconnect_the_present_run_is_counted_from_its_own_start);
    RUN_TEST(test_failed_disconnect_is_counted_as_failure_not_as_disconnect);
    RUN_TEST(test_applied_none_is_ignored);
    RUN_TEST(test_invalid_sample_never_connects_even_if_vbus_present_is_true);
    RUN_TEST(test_invalid_sample_breaks_a_connect_run);
    RUN_TEST(test_invalid_sample_counts_as_absent_when_connected);
    RUN_TEST(test_invalid_and_absent_samples_form_one_continuous_run);
    RUN_TEST(test_invalid_sample_in_the_middle_of_a_disconnected_absent_run_is_harmless);
    RUN_TEST(test_disabled_connects_once_on_first_update_then_none);
    RUN_TEST(test_disabled_ignores_input_and_clock);
    RUN_TEST(test_disabled_with_null_input_still_connects_once);
    RUN_TEST(test_disabled_does_not_retry_after_failure);
    RUN_TEST(test_disabled_ignores_debounce_values);
    RUN_TEST(test_clock_backward_restarts_the_disconnect_count_from_the_new_now);
    RUN_TEST(test_clock_backward_by_one_ms_restarts);
    RUN_TEST(test_clock_same_time_is_not_backward);
    RUN_TEST(test_clock_backward_restarts_the_connect_count);
    RUN_TEST(test_clock_backward_to_zero);
    RUN_TEST(test_clock_backward_while_not_counting_has_no_effect);
    RUN_TEST(test_repeated_backward_steps_delay_by_at_most_the_debounce_each);
    RUN_TEST(test_large_now_values_do_not_overflow);
    RUN_TEST(test_first_tick_with_large_now_does_not_look_like_a_backward_clock);
    RUN_TEST(test_null_arguments_do_not_crash_and_are_safe);
    RUN_TEST(test_null_input_is_an_invalid_sample_and_does_not_connect);
    RUN_TEST(test_null_input_counts_as_absent_when_connected);
    RUN_TEST(test_stats_count_connects_disconnects_failures);
    RUN_TEST(test_force_off_boundary_minus1_exact_plus1);
    RUN_TEST(test_force_off_wait_starts_at_the_first_disconnect);
    RUN_TEST(test_force_off_needs_a_disconnect_first_even_with_sparse_samples);
    RUN_TEST(test_force_off_ok_disconnects_and_counts_separately);
    RUN_TEST(test_force_off_failure_retries_every_tick_and_is_counted_as_a_failure);
    RUN_TEST(test_disconnect_success_ends_the_force_off_wait);
    RUN_TEST(test_vbus_return_restarts_the_force_off_wait);
    RUN_TEST(test_vbus_return_during_force_off_retry_cancels_it);
    RUN_TEST(test_invalid_samples_count_as_absent_for_force_off);
    RUN_TEST(test_clock_backward_restarts_the_force_off_wait);
    RUN_TEST(test_force_off_after_zero_is_one_ms);
    RUN_TEST(test_force_off_with_zero_disconnect_debounce);
    RUN_TEST(test_force_off_is_never_returned_when_disabled_or_disconnected);
    RUN_TEST(test_applied_force_off_with_null_is_safe);
    RUN_TEST(test_spec_removal_bound_for_fixed_sample_periods);
    RUN_TEST(test_spec_removal_bound_at_the_limits);
    RUN_TEST(test_spec_removal_bound_when_disconnect_succeeds);
    RUN_TEST(test_fuzz_monotone_against_reference_model_and_invariants);
    RUN_TEST(test_fuzz_non_monotone_clock_against_reference_model);
    RUN_TEST(test_fuzz_disabled_connects_exactly_once);
    return UNITY_END();
}
