/* video_codec_policy: H.264 の 5 条件、昇格の待ち（境界・途中の外れ）、即時の降格、接続 0 個、解像度の上限、
 * 時計の逆行、不整合な入力と NULL、許可の切替、ファズ（独立な参照モデルとの比較と不変条件）。
 * 契約は docs/VIDEO_TRANSPORT.md 9.5。結果の表は docs/VIDEO_CODEC_POLICY_TESTS.md。 */
#include "unity.h"
#include "video_codec_policy.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define MJPEG VIDEO_CODEC_POLICY_MJPEG
#define H264 VIDEO_CODEC_POLICY_H264
#define DWELL 5000u

void setUp(void) {}
void tearDown(void) {}

static video_codec_policy_config_t cfg(bool allowed, uint32_t dwell) {
    video_codec_policy_config_t c = video_codec_policy_config_default();
    c.h264_allowed = allowed;
    c.upgrade_dwell_ms = dwell;
    return c;
}

/* 5 条件がすべて満たされる入力（1 台のクライアントが申告、LOCKED、1920x1080、能力あり）。 */
static video_codec_policy_input_t good(uint64_t now) {
    video_codec_policy_input_t in;
    memset(&in, 0, sizeof(in));
    in.now_ms = now;
    in.source_supported_codecs = VIDEO_CODEC_POLICY_CAP_H264;
    in.client_count = 1;
    in.h264_client_count = 1;
    in.locked = true;
    in.width = 1920;
    in.height = 1080;
    return in;
}

static video_codec_policy_codec_t tick(video_codec_policy_t *p, const video_codec_policy_input_t *in) {
    return video_codec_policy_update(p, in);
}

static void init_allowed(video_codec_policy_t *p, uint32_t dwell) {
    video_codec_policy_config_t c = cfg(true, dwell);
    video_codec_policy_init(p, &c);
}

/* H.264 に上げる（t = 0 と t = DWELL）。戻り値は最後の now_ms。 */
static uint64_t promote(video_codec_policy_t *p) {
    video_codec_policy_input_t in = good(0);
    TEST_ASSERT_EQUAL_INT(MJPEG, tick(p, &in));
    in = good(DWELL);
    TEST_ASSERT_EQUAL_INT(H264, tick(p, &in));
    return DWELL;
}

/* ---- 外れる条件の表（1 つずつ外す）---- */
typedef enum {
    BRK_NOT_ALLOWED,
    BRK_NO_CAP,
    BRK_CAP_ONLY_MJPEG_BIT,
    BRK_NONE_DECLARED,      /* 1 台中 0 台が申告 */
    BRK_ONE_OF_TWO,         /* 2 台中 1 台だけが申告 */
    BRK_NOT_LOCKED,         /* 解像度は妥当なまま locked だけ偽 */
    BRK_WIDTH_ZERO,
    BRK_HEIGHT_ZERO,
    BRK_WIDTH_OVER,
    BRK_HEIGHT_OVER,
    BRK_INCONSISTENT,       /* h264_client_count > client_count */
    BRK_COUNT
} brk_t;

static const char *const BRK_NAME[BRK_COUNT] = {"not_allowed", "no_cap", "cap_only_mjpeg_bit", "none_declared", "one_of_two",
                                                 "not_locked", "width_zero", "height_zero", "width_over", "height_over",
                                                 "inconsistent"};

/* 「ハード」= 接続数に関係なく外れたら MJPEG（許可・能力・入力・不整合）。 */
static bool brk_is_hard(brk_t k) {
    return k != BRK_NONE_DECLARED && k != BRK_ONE_OF_TWO;
}

/* broken == true で条件を外し、false で戻す。BRK_NOT_ALLOWED だけは、policy の側を変える。 */
static void apply_brk(brk_t k, bool broken, video_codec_policy_t *p, video_codec_policy_input_t *in) {
    switch (k) {
    case BRK_NOT_ALLOWED: video_codec_policy_set_h264_allowed(p, !broken); break;
    case BRK_NO_CAP: if (broken) in->source_supported_codecs = 0; break;
    case BRK_CAP_ONLY_MJPEG_BIT: if (broken) in->source_supported_codecs = 1u << 0; break;
    case BRK_NONE_DECLARED: if (broken) in->h264_client_count = 0; break;
    case BRK_ONE_OF_TWO: in->client_count = 2; in->h264_client_count = broken ? 1 : 2; break;
    case BRK_NOT_LOCKED: if (broken) in->locked = false; break;
    case BRK_WIDTH_ZERO: if (broken) in->width = 0; break;
    case BRK_HEIGHT_ZERO: if (broken) in->height = 0; break;
    case BRK_WIDTH_OVER: if (broken) in->width = 1921; break;
    case BRK_HEIGHT_OVER: if (broken) in->height = 1081; break;
    case BRK_INCONSISTENT: if (broken) { in->client_count = 1; in->h264_client_count = 2; } break;
    default: break;
    }
}

/* ---------------- 初期状態・既定 ---------------- */

void test_config_default_values(void) {
    video_codec_policy_config_t c = video_codec_policy_config_default();
    TEST_ASSERT_FALSE(c.h264_allowed);
    TEST_ASSERT_EQUAL_UINT32(5000, c.upgrade_dwell_ms);
    TEST_ASSERT_EQUAL_UINT32(1920, c.max_width);
    TEST_ASSERT_EQUAL_UINT32(1080, c.max_height);
}

void test_init_is_mjpeg_with_zero_stats_and_keeps_config(void) {
    video_codec_policy_t p;
    memset(&p, 0xFF, sizeof(p));
    video_codec_policy_config_t c = cfg(true, 1234);
    c.max_width = 800;
    c.max_height = 600;
    video_codec_policy_init(&p, &c);
    TEST_ASSERT_EQUAL_INT(MJPEG, video_codec_policy_get_codec(&p));
    video_codec_policy_stats_t s = {7, 7};
    video_codec_policy_get_stats(&p, &s);
    TEST_ASSERT_EQUAL_UINT32(0, s.upgrades);
    TEST_ASSERT_EQUAL_UINT32(0, s.downgrades);
    TEST_ASSERT_TRUE(p.config.h264_allowed);
    TEST_ASSERT_EQUAL_UINT32(1234, p.config.upgrade_dwell_ms);
    TEST_ASSERT_EQUAL_UINT32(800, p.config.max_width);
    TEST_ASSERT_EQUAL_UINT32(600, p.config.max_height);
    TEST_ASSERT_FALSE(p.dwell_active);
    TEST_ASSERT_EQUAL_UINT64(0, p.last_now_ms);
}

void test_init_with_null_config_is_default_and_never_h264(void) {
    video_codec_policy_t p;
    video_codec_policy_init(&p, NULL);
    TEST_ASSERT_FALSE(p.config.h264_allowed);
    TEST_ASSERT_EQUAL_UINT32(5000, p.config.upgrade_dwell_ms);
    for (uint64_t t = 0; t <= 20000; t += 1000) {
        video_codec_policy_input_t in = good(t);
        TEST_ASSERT_EQUAL_INT(MJPEG, tick(&p, &in));
    }
}

/* ---------------- 許可 ---------------- */

void test_not_allowed_is_always_mjpeg_for_any_input(void) {
    video_codec_policy_t p;
    video_codec_policy_config_t c = cfg(false, 0); /* 待ちが 0 でも、許可がなければ上がらない */
    video_codec_policy_init(&p, &c);
    for (uint64_t t = 0; t <= 60000; t += 500) {
        video_codec_policy_input_t in = good(t);
        TEST_ASSERT_EQUAL_INT(MJPEG, tick(&p, &in));
    }
    video_codec_policy_stats_t s;
    video_codec_policy_get_stats(&p, &s);
    TEST_ASSERT_EQUAL_UINT32(0, s.upgrades);
}

/* ---------------- 1 条件ずつの否定: 長く待っても上がらない ---------------- */

void test_each_condition_alone_blocks_the_upgrade(void) {
    for (int k = 0; k < BRK_COUNT; k++) {
        video_codec_policy_t p;
        init_allowed(&p, DWELL);
        for (uint64_t t = 0; t <= 4 * DWELL; t += 1000) {
            video_codec_policy_input_t in = good(t);
            apply_brk((brk_t)k, true, &p, &in);
            char msg[64];
            snprintf(msg, sizeof(msg), "%s t=%llu", BRK_NAME[k], (unsigned long long)t);
            TEST_ASSERT_EQUAL_INT_MESSAGE(MJPEG, tick(&p, &in), msg);
        }
        video_codec_policy_stats_t s;
        video_codec_policy_get_stats(&p, &s);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, s.upgrades, BRK_NAME[k]);
    }
}

void test_zero_clients_blocks_the_upgrade(void) {
    video_codec_policy_t p;
    init_allowed(&p, DWELL);
    for (uint64_t t = 0; t <= 4 * DWELL; t += 1000) {
        video_codec_policy_input_t in = good(t);
        in.client_count = 0;
        in.h264_client_count = 0;
        TEST_ASSERT_EQUAL_INT(MJPEG, tick(&p, &in));
    }
}

void test_capability_mask_only_the_h264_bit_counts(void) {
    uint32_t masks[] = {0, 1u << 0, 1u << 2, 1u << 31, 0xFFFFFFFDu};
    for (size_t i = 0; i < sizeof(masks) / sizeof(masks[0]); i++) {
        video_codec_policy_t p;
        init_allowed(&p, 0);
        video_codec_policy_input_t in = good(0);
        in.source_supported_codecs = masks[i];
        TEST_ASSERT_EQUAL_INT_MESSAGE(MJPEG, tick(&p, &in), "mask without the H.264 bit");
    }
    uint32_t okmasks[] = {1u << 1, (1u << 0) | (1u << 1), 0xFFFFFFFFu};
    for (size_t i = 0; i < sizeof(okmasks) / sizeof(okmasks[0]); i++) {
        video_codec_policy_t p;
        init_allowed(&p, 0);
        video_codec_policy_input_t in = good(0);
        in.source_supported_codecs = okmasks[i];
        TEST_ASSERT_EQUAL_INT_MESSAGE(H264, tick(&p, &in), "mask with the H.264 bit");
    }
}

/* ---------------- 待ちの境界 ---------------- */

void test_upgrade_waits_exactly_dwell_ms_ticking_every_second(void) {
    video_codec_policy_t p;
    init_allowed(&p, DWELL);
    for (uint64_t t = 1000; t < 1000 + DWELL; t += 1000) {
        video_codec_policy_input_t in = good(t);
        TEST_ASSERT_EQUAL_INT(MJPEG, tick(&p, &in));
    }
    video_codec_policy_input_t in = good(1000 + DWELL);
    TEST_ASSERT_EQUAL_INT(H264, tick(&p, &in));
    video_codec_policy_stats_t s;
    video_codec_policy_get_stats(&p, &s);
    TEST_ASSERT_EQUAL_UINT32(1, s.upgrades);
    TEST_ASSERT_EQUAL_UINT32(0, s.downgrades);
}

void test_dwell_boundary_minus_one_exact_plus_one(void) {
    video_codec_policy_input_t in;
    video_codec_policy_t p;

    init_allowed(&p, DWELL); /* dwell - 1: まだ */
    in = good(1000);
    TEST_ASSERT_EQUAL_INT(MJPEG, tick(&p, &in));
    in = good(1000 + DWELL - 1);
    TEST_ASSERT_EQUAL_INT(MJPEG, tick(&p, &in));
    in = good(1000 + DWELL); /* 同じ policy で、次の tick がちょうど */
    TEST_ASSERT_EQUAL_INT(H264, tick(&p, &in));

    init_allowed(&p, DWELL); /* ちょうど: 上がる（経過 >= dwell） */
    in = good(1000);
    TEST_ASSERT_EQUAL_INT(MJPEG, tick(&p, &in));
    in = good(1000 + DWELL);
    TEST_ASSERT_EQUAL_INT(H264, tick(&p, &in));

    init_allowed(&p, DWELL); /* dwell + 1: 上がる */
    in = good(1000);
    TEST_ASSERT_EQUAL_INT(MJPEG, tick(&p, &in));
    in = good(1000 + DWELL + 1);
    TEST_ASSERT_EQUAL_INT(H264, tick(&p, &in));
}

void test_dwell_zero_upgrades_on_the_first_satisfied_tick(void) {
    video_codec_policy_t p;
    init_allowed(&p, 0);
    video_codec_policy_input_t in = good(12345);
    TEST_ASSERT_EQUAL_INT(H264, tick(&p, &in));
}

void test_dwell_one_boundary(void) {
    video_codec_policy_t p;
    init_allowed(&p, 1);
    video_codec_policy_input_t in = good(100);
    TEST_ASSERT_EQUAL_INT(MJPEG, tick(&p, &in)); /* 経過 0 < 1 */
    in = good(101);
    TEST_ASSERT_EQUAL_INT(H264, tick(&p, &in));
}

void test_dwell_counts_from_the_first_satisfied_tick_not_from_init(void) {
    video_codec_policy_t p;
    init_allowed(&p, DWELL);
    video_codec_policy_input_t in = good(0);
    in.locked = false;
    TEST_ASSERT_EQUAL_INT(MJPEG, tick(&p, &in));
    in = good(100000); /* 条件が初めて満たされた */
    TEST_ASSERT_EQUAL_INT(MJPEG, tick(&p, &in));
    in = good(100000 + DWELL - 1);
    TEST_ASSERT_EQUAL_INT(MJPEG, tick(&p, &in));
    in = good(100000 + DWELL);
    TEST_ASSERT_EQUAL_INT(H264, tick(&p, &in));
}

void test_dwell_is_not_extended_by_more_satisfied_ticks(void) {
    /* 満たされている間は、tick が増えても、始まりの時刻は動かない */
    video_codec_policy_t p;
    init_allowed(&p, DWELL);
    for (uint64_t t = 0; t < DWELL; t += 100) {
        video_codec_policy_input_t in = good(t);
        TEST_ASSERT_EQUAL_INT(MJPEG, tick(&p, &in));
    }
    video_codec_policy_input_t in = good(DWELL);
    TEST_ASSERT_EQUAL_INT(H264, tick(&p, &in));
}

/* ---------------- 待ちの途中の外れ: 最初からやり直す ---------------- */

void test_any_single_break_restarts_the_dwell(void) {
    for (int k = 0; k < BRK_COUNT; k++) {
        video_codec_policy_t p;
        init_allowed(&p, DWELL);
        video_codec_policy_input_t in = good(0);
        TEST_ASSERT_EQUAL_INT(MJPEG, tick(&p, &in));
        in = good(DWELL - 1000); /* 待ちの最後の手前 */
        TEST_ASSERT_EQUAL_INT(MJPEG, tick(&p, &in));
        in = good(DWELL - 500); /* 1 tick だけ外れる */
        apply_brk((brk_t)k, true, &p, &in);
        TEST_ASSERT_EQUAL_INT_MESSAGE(MJPEG, tick(&p, &in), BRK_NAME[k]);
        in = good(DWELL); /* 戻る。ここが新しい始まり */
        apply_brk((brk_t)k, false, &p, &in);
        TEST_ASSERT_EQUAL_INT_MESSAGE(MJPEG, tick(&p, &in), BRK_NAME[k]);
        in = good(DWELL + DWELL - 1);
        apply_brk((brk_t)k, false, &p, &in);
        TEST_ASSERT_EQUAL_INT_MESSAGE(MJPEG, tick(&p, &in), BRK_NAME[k]);
        in = good(DWELL + DWELL);
        apply_brk((brk_t)k, false, &p, &in);
        TEST_ASSERT_EQUAL_INT_MESSAGE(H264, tick(&p, &in), BRK_NAME[k]);
    }
}

void test_break_at_the_very_tick_the_dwell_would_complete_prevents_the_upgrade(void) {
    video_codec_policy_t p;
    init_allowed(&p, DWELL);
    video_codec_policy_input_t in = good(0);
    TEST_ASSERT_EQUAL_INT(MJPEG, tick(&p, &in));
    in = good(DWELL);
    in.locked = false;
    TEST_ASSERT_EQUAL_INT(MJPEG, tick(&p, &in));
    in = good(DWELL + 1); /* 戻っても、始まりは DWELL + 1 */
    TEST_ASSERT_EQUAL_INT(MJPEG, tick(&p, &in));
    in = good(DWELL + 1 + DWELL - 1);
    TEST_ASSERT_EQUAL_INT(MJPEG, tick(&p, &in));
    in = good(DWELL + 1 + DWELL);
    TEST_ASSERT_EQUAL_INT(H264, tick(&p, &in));
}

void test_second_client_without_declaration_joining_then_leaving_restarts_the_dwell(void) {
    video_codec_policy_t p;
    init_allowed(&p, DWELL);
    video_codec_policy_input_t in = good(0);
    TEST_ASSERT_EQUAL_INT(MJPEG, tick(&p, &in));
    in = good(3000);
    TEST_ASSERT_EQUAL_INT(MJPEG, tick(&p, &in));
    in = good(3500); /* 古いビューアが接続した */
    in.client_count = 2;
    in.h264_client_count = 1;
    TEST_ASSERT_EQUAL_INT(MJPEG, tick(&p, &in));
    in = good(4000); /* 出ていった。ここから数え直す */
    TEST_ASSERT_EQUAL_INT(MJPEG, tick(&p, &in));
    in = good(DWELL + 3999); /* 元の始まり（0）から 5000 以上経っているが、まだ */
    TEST_ASSERT_EQUAL_INT(MJPEG, tick(&p, &in));
    in = good(4000 + DWELL);
    TEST_ASSERT_EQUAL_INT(H264, tick(&p, &in));
}

void test_two_declared_clients_upgrade(void) {
    video_codec_policy_t p;
    init_allowed(&p, DWELL);
    video_codec_policy_input_t in = good(0);
    in.client_count = 2;
    in.h264_client_count = 2;
    TEST_ASSERT_EQUAL_INT(MJPEG, tick(&p, &in));
    in.now_ms = DWELL;
    TEST_ASSERT_EQUAL_INT(H264, tick(&p, &in));
}

/* ---------------- 即時の降格 ---------------- */

void test_every_hard_condition_downgrades_at_once_from_h264(void) {
    for (int k = 0; k < BRK_COUNT; k++) {
        if (!brk_is_hard((brk_t)k)) continue;
        video_codec_policy_t p;
        init_allowed(&p, DWELL);
        uint64_t t = promote(&p);
        video_codec_policy_input_t in = good(t + 1); /* 1 ms 後でも、即時 */
        apply_brk((brk_t)k, true, &p, &in);
        TEST_ASSERT_EQUAL_INT_MESSAGE(MJPEG, tick(&p, &in), BRK_NAME[k]);
        TEST_ASSERT_EQUAL_INT_MESSAGE(MJPEG, video_codec_policy_get_codec(&p), BRK_NAME[k]);
        video_codec_policy_stats_t s;
        video_codec_policy_get_stats(&p, &s);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, s.upgrades, BRK_NAME[k]);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, s.downgrades, BRK_NAME[k]);
    }
}

void test_hard_conditions_downgrade_even_with_zero_clients(void) {
    for (int k = 0; k < BRK_COUNT; k++) {
        if (!brk_is_hard((brk_t)k) || k == BRK_INCONSISTENT) continue;
        video_codec_policy_t p;
        init_allowed(&p, DWELL);
        uint64_t t = promote(&p);
        video_codec_policy_input_t in = good(t + 1);
        apply_brk((brk_t)k, true, &p, &in);
        in.client_count = 0;
        in.h264_client_count = 0;
        TEST_ASSERT_EQUAL_INT_MESSAGE(MJPEG, tick(&p, &in), BRK_NAME[k]);
    }
}

void test_undeclared_client_downgrades_at_once(void) {
    video_codec_policy_t p;
    init_allowed(&p, DWELL);
    uint64_t t = promote(&p);
    video_codec_policy_input_t in = good(t + 1); /* 1 台、申告なし */
    in.h264_client_count = 0;
    TEST_ASSERT_EQUAL_INT(MJPEG, tick(&p, &in));
}

void test_one_of_two_clients_undeclared_downgrades_at_once(void) {
    video_codec_policy_t p;
    init_allowed(&p, DWELL);
    uint64_t t = promote(&p);
    video_codec_policy_input_t in = good(t + 1);
    in.client_count = 2;
    in.h264_client_count = 1;
    TEST_ASSERT_EQUAL_INT(MJPEG, tick(&p, &in));
    in = good(t + 2); /* 申告なしが出ていっても、すぐには戻らない（待ちから） */
    TEST_ASSERT_EQUAL_INT(MJPEG, tick(&p, &in));
}

void test_h264_stays_while_all_conditions_hold(void) {
    video_codec_policy_t p;
    init_allowed(&p, DWELL);
    uint64_t t = promote(&p);
    for (int i = 0; i < 100; i++) {
        t += 1000;
        video_codec_policy_input_t in = good(t);
        TEST_ASSERT_EQUAL_INT(H264, tick(&p, &in));
    }
    video_codec_policy_stats_t s;
    video_codec_policy_get_stats(&p, &s);
    TEST_ASSERT_EQUAL_UINT32(1, s.upgrades);
    TEST_ASSERT_EQUAL_UINT32(0, s.downgrades);
}

void test_h264_stays_with_a_second_declared_client_joining(void) {
    video_codec_policy_t p;
    init_allowed(&p, DWELL);
    uint64_t t = promote(&p);
    video_codec_policy_input_t in = good(t + 10);
    in.client_count = 2;
    in.h264_client_count = 2;
    TEST_ASSERT_EQUAL_INT(H264, tick(&p, &in));
}

void test_after_a_downgrade_the_dwell_starts_over(void) {
    video_codec_policy_t p;
    init_allowed(&p, DWELL);
    uint64_t t = promote(&p);
    video_codec_policy_input_t in = good(t + 1000);
    in.locked = false;
    TEST_ASSERT_EQUAL_INT(MJPEG, tick(&p, &in));
    in = good(t + 2000); /* 条件が戻った tick が新しい始まり */
    TEST_ASSERT_EQUAL_INT(MJPEG, tick(&p, &in));
    in = good(t + 2000 + DWELL - 1);
    TEST_ASSERT_EQUAL_INT(MJPEG, tick(&p, &in));
    in = good(t + 2000 + DWELL);
    TEST_ASSERT_EQUAL_INT(H264, tick(&p, &in));
    video_codec_policy_stats_t s;
    video_codec_policy_get_stats(&p, &s);
    TEST_ASSERT_EQUAL_UINT32(2, s.upgrades);
    TEST_ASSERT_EQUAL_UINT32(1, s.downgrades);
}

void test_stale_dwell_is_not_reused_after_upgrade_then_downgrade(void) {
    /* 昇格のときの待ちの始まり（0）が、降格のあとに残っていると、戻った直後に即昇格してしまう */
    video_codec_policy_t p;
    init_allowed(&p, DWELL);
    uint64_t t = promote(&p);
    video_codec_policy_input_t in = good(t + 1);
    in.locked = false;
    TEST_ASSERT_EQUAL_INT(MJPEG, tick(&p, &in));
    in = good(t + 2);
    TEST_ASSERT_EQUAL_INT(MJPEG, tick(&p, &in));
}

/* ---------------- 接続が 0 個 ---------------- */

void test_zero_clients_keeps_h264(void) {
    video_codec_policy_t p;
    init_allowed(&p, DWELL);
    uint64_t t = promote(&p);
    for (int i = 0; i < 20; i++) {
        t += 3000;
        video_codec_policy_input_t in = good(t);
        in.client_count = 0;
        in.h264_client_count = 0;
        TEST_ASSERT_EQUAL_INT(H264, tick(&p, &in));
    }
    video_codec_policy_stats_t s;
    video_codec_policy_get_stats(&p, &s);
    TEST_ASSERT_EQUAL_UINT32(0, s.downgrades);
}

void test_zero_clients_keeps_mjpeg(void) {
    video_codec_policy_t p;
    init_allowed(&p, DWELL);
    for (uint64_t t = 0; t < 10 * DWELL; t += 1000) {
        video_codec_policy_input_t in = good(t);
        in.client_count = 0;
        in.h264_client_count = 0;
        TEST_ASSERT_EQUAL_INT(MJPEG, tick(&p, &in));
    }
}

void test_h264_after_zero_clients_is_judged_again_by_the_next_connection(void) {
    video_codec_policy_t p;
    init_allowed(&p, DWELL);
    uint64_t t = promote(&p);
    video_codec_policy_input_t in = good(t + 1000);
    in.client_count = 0;
    in.h264_client_count = 0;
    TEST_ASSERT_EQUAL_INT(H264, tick(&p, &in));
    in = good(t + 2000); /* 申告のないビューアが接続: 即 MJPEG */
    in.h264_client_count = 0;
    TEST_ASSERT_EQUAL_INT(MJPEG, tick(&p, &in));
}

void test_zero_clients_discards_the_dwell(void) {
    video_codec_policy_t p;
    init_allowed(&p, DWELL);
    video_codec_policy_input_t in = good(0);
    TEST_ASSERT_EQUAL_INT(MJPEG, tick(&p, &in));
    in = good(4000);
    TEST_ASSERT_EQUAL_INT(MJPEG, tick(&p, &in));
    in = good(4500); /* 全員が切断 */
    in.client_count = 0;
    in.h264_client_count = 0;
    TEST_ASSERT_EQUAL_INT(MJPEG, tick(&p, &in));
    in = good(5000); /* 新しい接続。ここから数える */
    TEST_ASSERT_EQUAL_INT(MJPEG, tick(&p, &in));
    in = good(5000 + DWELL - 1);
    TEST_ASSERT_EQUAL_INT(MJPEG, tick(&p, &in));
    in = good(5000 + DWELL);
    TEST_ASSERT_EQUAL_INT(H264, tick(&p, &in));
}

/* ---------------- 解像度の上限 ---------------- */

static video_codec_policy_codec_t res_result(const video_codec_policy_config_t *c, uint32_t w, uint32_t h) {
    video_codec_policy_t p;
    video_codec_policy_init(&p, c);
    video_codec_policy_input_t in = good(0);
    in.width = w;
    in.height = h;
    return tick(&p, &in);
}

void test_resolution_limit_boundaries_with_defaults(void) {
    video_codec_policy_config_t c = cfg(true, 0);
    TEST_ASSERT_EQUAL_INT(H264, res_result(&c, 1920, 1080));
    TEST_ASSERT_EQUAL_INT(MJPEG, res_result(&c, 1921, 1080));
    TEST_ASSERT_EQUAL_INT(MJPEG, res_result(&c, 1920, 1081));
    TEST_ASSERT_EQUAL_INT(MJPEG, res_result(&c, 1921, 1081));
    TEST_ASSERT_EQUAL_INT(H264, res_result(&c, 1919, 1079));
    TEST_ASSERT_EQUAL_INT(H264, res_result(&c, 1280, 720));
    TEST_ASSERT_EQUAL_INT(H264, res_result(&c, 640, 480));
    TEST_ASSERT_EQUAL_INT(H264, res_result(&c, 1, 1));
    TEST_ASSERT_EQUAL_INT(MJPEG, res_result(&c, 0, 1080));
    TEST_ASSERT_EQUAL_INT(MJPEG, res_result(&c, 1920, 0));
    TEST_ASSERT_EQUAL_INT(MJPEG, res_result(&c, 0, 0));
    TEST_ASSERT_EQUAL_INT(MJPEG, res_result(&c, UINT32_MAX, 1080));
    TEST_ASSERT_EQUAL_INT(MJPEG, res_result(&c, 1920, UINT32_MAX));
    TEST_ASSERT_EQUAL_INT(MJPEG, res_result(&c, 3840, 2160));
    /* 縦横を取り違えても、上限は別々に効く */
    TEST_ASSERT_EQUAL_INT(MJPEG, res_result(&c, 1080, 1921));
    TEST_ASSERT_EQUAL_INT(H264, res_result(&c, 1080, 1080));
}

void test_resolution_limit_follows_the_config(void) {
    video_codec_policy_config_t c = cfg(true, 0);
    c.max_width = 1280;
    c.max_height = 720;
    TEST_ASSERT_EQUAL_INT(H264, res_result(&c, 1280, 720));
    TEST_ASSERT_EQUAL_INT(MJPEG, res_result(&c, 1281, 720));
    TEST_ASSERT_EQUAL_INT(MJPEG, res_result(&c, 1280, 721));
    TEST_ASSERT_EQUAL_INT(MJPEG, res_result(&c, 1920, 1080));
    c.max_width = 4096; /* 幅と高さの上限は独立 */
    c.max_height = 720;
    TEST_ASSERT_EQUAL_INT(H264, res_result(&c, 4096, 720));
    TEST_ASSERT_EQUAL_INT(MJPEG, res_result(&c, 4096, 721));
    c.max_width = 1280;
    c.max_height = 4096;
    TEST_ASSERT_EQUAL_INT(H264, res_result(&c, 1280, 4096));
    TEST_ASSERT_EQUAL_INT(MJPEG, res_result(&c, 1281, 4096));
}

void test_zero_limit_allows_no_resolution(void) {
    video_codec_policy_config_t c = cfg(true, 0);
    c.max_width = 0;
    TEST_ASSERT_EQUAL_INT(MJPEG, res_result(&c, 1, 1));
    c.max_width = 1920;
    c.max_height = 0;
    TEST_ASSERT_EQUAL_INT(MJPEG, res_result(&c, 1, 1));
}

void test_resolution_change_over_the_limit_downgrades_and_back_within_waits(void) {
    video_codec_policy_t p;
    init_allowed(&p, DWELL);
    uint64_t t = promote(&p);
    video_codec_policy_input_t in = good(t + 1);
    in.width = 3840;
    in.height = 2160;
    TEST_ASSERT_EQUAL_INT(MJPEG, tick(&p, &in));
    in = good(t + 2);
    TEST_ASSERT_EQUAL_INT(MJPEG, tick(&p, &in));
    in = good(t + 2 + DWELL);
    TEST_ASSERT_EQUAL_INT(H264, tick(&p, &in));
}

void test_locked_false_ignores_the_reported_resolution(void) {
    /* locked でなければ、解像度が上限内でも、H.264 にならない（逆に、上限外でも理由は locked） */
    video_codec_policy_t p;
    init_allowed(&p, 0);
    video_codec_policy_input_t in = good(0);
    in.locked = false;
    TEST_ASSERT_EQUAL_INT(MJPEG, tick(&p, &in));
    in.width = 0;
    in.height = 0;
    TEST_ASSERT_EQUAL_INT(MJPEG, tick(&p, &in));
}

/* ---------------- 時計の逆行・オーバーフロー ---------------- */

void test_clock_regression_restarts_the_dwell(void) {
    video_codec_policy_t p;
    init_allowed(&p, DWELL);
    video_codec_policy_input_t in = good(0);
    TEST_ASSERT_EQUAL_INT(MJPEG, tick(&p, &in));
    in = good(3000);
    TEST_ASSERT_EQUAL_INT(MJPEG, tick(&p, &in));
    in = good(2000); /* 戻った。始まりは、ここ（2000） */
    TEST_ASSERT_EQUAL_INT(MJPEG, tick(&p, &in));
    in = good(DWELL); /* 始まりが 0 のままなら、ここで上がってしまう */
    TEST_ASSERT_EQUAL_INT(MJPEG, tick(&p, &in));
    in = good(2000 + DWELL - 1);
    TEST_ASSERT_EQUAL_INT(MJPEG, tick(&p, &in));
    in = good(2000 + DWELL);
    TEST_ASSERT_EQUAL_INT(H264, tick(&p, &in));
}

void test_clock_regression_below_the_dwell_start_does_not_wrap_around(void) {
    video_codec_policy_t p;
    init_allowed(&p, DWELL);
    video_codec_policy_input_t in = good(1000000);
    TEST_ASSERT_EQUAL_INT(MJPEG, tick(&p, &in));
    in = good(5); /* 始まりより小さい。引き算が巻き戻ると、巨大な経過になる */
    TEST_ASSERT_EQUAL_INT(MJPEG, tick(&p, &in));
    in = good(5 + DWELL - 1);
    TEST_ASSERT_EQUAL_INT(MJPEG, tick(&p, &in));
    in = good(5 + DWELL);
    TEST_ASSERT_EQUAL_INT(H264, tick(&p, &in));
}

void test_clock_regression_to_zero_at_the_64bit_limit(void) {
    video_codec_policy_t p;
    init_allowed(&p, DWELL);
    video_codec_policy_input_t in = good(UINT64_MAX - 100);
    TEST_ASSERT_EQUAL_INT(MJPEG, tick(&p, &in));
    in = good(0);
    TEST_ASSERT_EQUAL_INT(MJPEG, tick(&p, &in));
    in = good(DWELL - 1);
    TEST_ASSERT_EQUAL_INT(MJPEG, tick(&p, &in));
    in = good(DWELL);
    TEST_ASSERT_EQUAL_INT(H264, tick(&p, &in));
}

void test_clock_regression_while_broken_does_not_affect_the_next_dwell(void) {
    video_codec_policy_t p;
    init_allowed(&p, DWELL);
    video_codec_policy_input_t in = good(50000);
    in.locked = false;
    TEST_ASSERT_EQUAL_INT(MJPEG, tick(&p, &in));
    in = good(10); /* 逆行しているが、待ちの最中ではない */
    in.locked = false;
    TEST_ASSERT_EQUAL_INT(MJPEG, tick(&p, &in));
    in = good(20);
    TEST_ASSERT_EQUAL_INT(MJPEG, tick(&p, &in));
    in = good(20 + DWELL);
    TEST_ASSERT_EQUAL_INT(H264, tick(&p, &in));
}

void test_clock_regression_in_h264_does_not_downgrade(void) {
    video_codec_policy_t p;
    init_allowed(&p, DWELL);
    uint64_t t = promote(&p);
    video_codec_policy_input_t in = good(t - 3000);
    TEST_ASSERT_EQUAL_INT(H264, tick(&p, &in));
    in = good(0);
    TEST_ASSERT_EQUAL_INT(H264, tick(&p, &in));
    in = good(0);
    in.locked = false; /* 逆行していても、条件の外れでは降格する */
    TEST_ASSERT_EQUAL_INT(MJPEG, tick(&p, &in));
}

void test_equal_now_ms_is_not_a_regression(void) {
    video_codec_policy_t p;
    init_allowed(&p, DWELL);
    video_codec_policy_input_t in = good(1000);
    TEST_ASSERT_EQUAL_INT(MJPEG, tick(&p, &in));
    in = good(2000);
    TEST_ASSERT_EQUAL_INT(MJPEG, tick(&p, &in));
    in = good(2000); /* 同じ時刻の tick（逆行ではない）。始まりは 1000 のまま */
    TEST_ASSERT_EQUAL_INT(MJPEG, tick(&p, &in));
    in = good(1000 + DWELL);
    TEST_ASSERT_EQUAL_INT(H264, tick(&p, &in));
}

void test_regression_by_one_ms_restarts_the_dwell(void) {
    video_codec_policy_t p;
    init_allowed(&p, DWELL);
    video_codec_policy_input_t in = good(1000);
    TEST_ASSERT_EQUAL_INT(MJPEG, tick(&p, &in));
    in = good(1999);
    TEST_ASSERT_EQUAL_INT(MJPEG, tick(&p, &in));
    in = good(1998); /* 1 ms の逆行 */
    TEST_ASSERT_EQUAL_INT(MJPEG, tick(&p, &in));
    in = good(1000 + DWELL); /* 始まりが 1000 のままなら上がる。1998 なら、まだ */
    TEST_ASSERT_EQUAL_INT(MJPEG, tick(&p, &in));
    in = good(1998 + DWELL);
    TEST_ASSERT_EQUAL_INT(H264, tick(&p, &in));
}

void test_large_now_values_do_not_overflow(void) {
    video_codec_policy_t p;
    init_allowed(&p, DWELL);
    uint64_t base = UINT64_MAX - DWELL;
    video_codec_policy_input_t in = good(base);
    TEST_ASSERT_EQUAL_INT(MJPEG, tick(&p, &in));
    in = good(base + DWELL - 1);
    TEST_ASSERT_EQUAL_INT(MJPEG, tick(&p, &in));
    in = good(UINT64_MAX); /* base + DWELL */
    TEST_ASSERT_EQUAL_INT(H264, tick(&p, &in));
    init_allowed(&p, DWELL); /* 32 ビットを超える時刻（49 日以上）でも同じ */
    uint64_t big = 0x100000000ull * 3 + 10;
    in = good(big);
    TEST_ASSERT_EQUAL_INT(MJPEG, tick(&p, &in));
    in = good(big + DWELL - 1);
    TEST_ASSERT_EQUAL_INT(MJPEG, tick(&p, &in));
    in = good(big + DWELL);
    TEST_ASSERT_EQUAL_INT(H264, tick(&p, &in));
    init_allowed(&p, DWELL); /* 経過が 32 ビットを超える（2^32 ms = 約 49.7 日。tick が止まっていた場合）。切り詰めない */
    in = good(10);
    TEST_ASSERT_EQUAL_INT(MJPEG, tick(&p, &in));
    in = good(10 + 0x100000000ull);
    TEST_ASSERT_EQUAL_INT(H264, tick(&p, &in));
    init_allowed(&p, DWELL);
    in = good(0);
    TEST_ASSERT_EQUAL_INT(MJPEG, tick(&p, &in));
    in = good(0x100000000ull + DWELL - 1); /* 32 ビットに切り詰めると dwell - 1 で、境界の外側と区別できない */
    TEST_ASSERT_EQUAL_INT(H264, tick(&p, &in));
}

/* ---------------- 不整合な入力・NULL ---------------- */

void test_inconsistent_counts_never_upgrade(void) {
    video_codec_policy_t p;
    init_allowed(&p, 0);
    video_codec_policy_input_t in = good(0);
    in.client_count = 1;
    in.h264_client_count = 2;
    TEST_ASSERT_EQUAL_INT(MJPEG, tick(&p, &in));
    in = good(1);
    in.client_count = 0;
    in.h264_client_count = 1;
    TEST_ASSERT_EQUAL_INT(MJPEG, tick(&p, &in));
    in = good(2);
    in.client_count = 0;
    in.h264_client_count = UINT32_MAX;
    TEST_ASSERT_EQUAL_INT(MJPEG, tick(&p, &in));
    in = good(3);
    in.client_count = 2;
    in.h264_client_count = 3;
    TEST_ASSERT_EQUAL_INT(MJPEG, tick(&p, &in));
}

void test_inconsistent_counts_downgrade_h264_even_with_zero_clients(void) {
    video_codec_policy_t p;
    init_allowed(&p, DWELL);
    uint64_t t = promote(&p);
    video_codec_policy_input_t in = good(t + 1);
    in.client_count = 0;
    in.h264_client_count = 1; /* client_count == 0 の「変えない」より、不整合の安全側が先 */
    TEST_ASSERT_EQUAL_INT(MJPEG, tick(&p, &in));
}

void test_inconsistent_input_discards_the_dwell(void) {
    video_codec_policy_t p;
    init_allowed(&p, DWELL);
    video_codec_policy_input_t in = good(0);
    TEST_ASSERT_EQUAL_INT(MJPEG, tick(&p, &in));
    in = good(3000);
    in.client_count = 1;
    in.h264_client_count = 2;
    TEST_ASSERT_EQUAL_INT(MJPEG, tick(&p, &in));
    in = good(DWELL);
    TEST_ASSERT_EQUAL_INT(MJPEG, tick(&p, &in));
}

void test_null_arguments_do_not_crash_and_are_safe(void) {
    video_codec_policy_input_t in = good(0);
    video_codec_policy_init(NULL, NULL);
    video_codec_policy_set_h264_allowed(NULL, true);
    TEST_ASSERT_EQUAL_INT(MJPEG, video_codec_policy_update(NULL, NULL));
    TEST_ASSERT_EQUAL_INT(MJPEG, video_codec_policy_update(NULL, &in));
    TEST_ASSERT_EQUAL_INT(MJPEG, video_codec_policy_get_codec(NULL));
    video_codec_policy_stats_t s = {9, 9};
    video_codec_policy_get_stats(NULL, &s);
    TEST_ASSERT_EQUAL_UINT32(0, s.upgrades);
    TEST_ASSERT_EQUAL_UINT32(0, s.downgrades);
    video_codec_policy_t p;
    init_allowed(&p, DWELL);
    video_codec_policy_get_stats(&p, NULL);
}

void test_null_input_downgrades_h264_and_discards_the_dwell(void) {
    video_codec_policy_t p;
    init_allowed(&p, DWELL);
    video_codec_policy_input_t in = good(0);
    TEST_ASSERT_EQUAL_INT(MJPEG, tick(&p, &in));
    TEST_ASSERT_EQUAL_INT(MJPEG, tick(&p, NULL));
    in = good(DWELL); /* NULL の tick で待ちは捨てられた。ここが新しい始まり */
    TEST_ASSERT_EQUAL_INT(MJPEG, tick(&p, &in));
    in = good(2 * DWELL);
    TEST_ASSERT_EQUAL_INT(H264, tick(&p, &in));
    TEST_ASSERT_EQUAL_INT(MJPEG, tick(&p, NULL));
    TEST_ASSERT_EQUAL_INT(MJPEG, video_codec_policy_get_codec(&p));
    video_codec_policy_stats_t s;
    video_codec_policy_get_stats(&p, &s);
    TEST_ASSERT_EQUAL_UINT32(1, s.downgrades);
}

/* ---------------- 許可の切替 ---------------- */

void test_allowed_switched_off_downgrades_even_with_zero_clients(void) {
    video_codec_policy_t p;
    init_allowed(&p, DWELL);
    uint64_t t = promote(&p);
    video_codec_policy_set_h264_allowed(&p, false);
    TEST_ASSERT_EQUAL_INT(H264, video_codec_policy_get_codec(&p)); /* update するまで変わらない */
    video_codec_policy_input_t in = good(t + 1);
    in.client_count = 0;
    in.h264_client_count = 0;
    TEST_ASSERT_EQUAL_INT(MJPEG, tick(&p, &in));
}

void test_allowed_switched_on_again_waits_the_dwell(void) {
    video_codec_policy_t p;
    init_allowed(&p, DWELL);
    uint64_t t = promote(&p);
    video_codec_policy_set_h264_allowed(&p, false);
    video_codec_policy_input_t in = good(t + 1);
    TEST_ASSERT_EQUAL_INT(MJPEG, tick(&p, &in));
    video_codec_policy_set_h264_allowed(&p, true);
    in = good(t + 2);
    TEST_ASSERT_EQUAL_INT(MJPEG, tick(&p, &in));
    in = good(t + 2 + DWELL - 1);
    TEST_ASSERT_EQUAL_INT(MJPEG, tick(&p, &in));
    in = good(t + 2 + DWELL);
    TEST_ASSERT_EQUAL_INT(H264, tick(&p, &in));
}

void test_allowed_off_during_the_dwell_restarts_it(void) {
    video_codec_policy_t p;
    init_allowed(&p, DWELL);
    video_codec_policy_input_t in = good(0);
    TEST_ASSERT_EQUAL_INT(MJPEG, tick(&p, &in));
    video_codec_policy_set_h264_allowed(&p, false);
    in = good(1000);
    TEST_ASSERT_EQUAL_INT(MJPEG, tick(&p, &in));
    video_codec_policy_set_h264_allowed(&p, true);
    in = good(2000);
    TEST_ASSERT_EQUAL_INT(MJPEG, tick(&p, &in));
    in = good(DWELL); /* 元の始まり（0）からなら上がる */
    TEST_ASSERT_EQUAL_INT(MJPEG, tick(&p, &in));
    in = good(2000 + DWELL);
    TEST_ASSERT_EQUAL_INT(H264, tick(&p, &in));
}

/* ---------------- 統計 ---------------- */

void test_stats_count_each_transition_once(void) {
    video_codec_policy_t p;
    init_allowed(&p, 0);
    video_codec_policy_stats_t s;
    uint64_t t = 0;
    for (int i = 0; i < 3; i++) {
        video_codec_policy_input_t in = good(t++);
        TEST_ASSERT_EQUAL_INT(H264, tick(&p, &in));
        in = good(t++); /* 上がったままの tick は数えない */
        TEST_ASSERT_EQUAL_INT(H264, tick(&p, &in));
        in = good(t++);
        in.locked = false;
        TEST_ASSERT_EQUAL_INT(MJPEG, tick(&p, &in));
        in = good(t++);
        in.locked = false; /* 下がったままの tick も数えない */
        TEST_ASSERT_EQUAL_INT(MJPEG, tick(&p, &in));
    }
    video_codec_policy_get_stats(&p, &s);
    TEST_ASSERT_EQUAL_UINT32(3, s.upgrades);
    TEST_ASSERT_EQUAL_UINT32(3, s.downgrades);
}

/* ---------------- ファズ: 独立な参照モデルとの比較と不変条件 ---------------- */

static uint64_t g_rng;
static uint32_t rnd(void) {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return (uint32_t)(g_rng >> 16);
}
static bool chance(uint32_t pct) { return (rnd() % 100u) < pct; }

typedef struct {
    int codec;
    bool streak;
    uint64_t streak_start;
    bool have_last;
    uint64_t last_now;
} model_t;

/* 参照モデル: 実装とは別の書き方（連続の始まりを持ち、条件を 1 つの式で評価する）。 */
static int model_step(model_t *m, bool allowed, uint32_t dwell, const video_codec_policy_input_t *in) {
    bool cap = (in->source_supported_codecs & 2u) != 0;
    bool consistent = in->h264_client_count <= in->client_count;
    bool input_ok = in->locked && in->width >= 1 && in->width <= 1920 && in->height >= 1 && in->height <= 1080;
    bool hard = allowed && cap && input_ok && consistent;
    bool all_declared = in->client_count >= 1 && in->h264_client_count == in->client_count;
    bool regressed = m->have_last && in->now_ms < m->last_now;
    m->have_last = true;
    m->last_now = in->now_ms;

    if (m->codec == 1) {
        m->streak = false;
        if (!hard) m->codec = 0;
        else if (in->client_count >= 1 && !all_declared) m->codec = 0;
        return m->codec;
    }
    if (hard && all_declared) {
        if (!m->streak) {
            m->streak = true;
            m->streak_start = in->now_ms;
        } else if (regressed) {
            m->streak_start = in->now_ms;
        }
        if (in->now_ms - m->streak_start >= dwell) m->codec = 1;
    } else {
        m->streak = false;
    }
    return m->codec;
}

static void fuzz_one(uint64_t seed, int steps, uint32_t *upgrades_seen, uint32_t *downgrades_seen) {
    g_rng = seed * 0x9E3779B97F4A7C15ull + 1;
    uint32_t dwell_choices[] = {0, 1, 500, 2000, 5000};
    uint32_t dwell = dwell_choices[rnd() % 5];
    bool allowed = chance(90);
    video_codec_policy_t p;
    video_codec_policy_config_t c = cfg(allowed, dwell);
    video_codec_policy_init(&p, &c);
    model_t m;
    memset(&m, 0, sizeof(m));
    uint64_t now = chance(10) ? (UINT64_MAX - 5000000) : (uint64_t)rnd() * 3;
    int prev = 0;
    for (int i = 0; i < steps; i++) {
        char msg[96];
        snprintf(msg, sizeof(msg), "seed=%llu step=%d", (unsigned long long)seed, i);
        if (chance(1)) { /* 許可の切替 */
            allowed = !allowed;
            video_codec_policy_set_h264_allowed(&p, allowed);
        }
        uint32_t r = rnd() % 100u;
        if (r < 3) {
            uint32_t d = rnd() % 4000u; /* 逆行（下限は 0） */
            now = d < now ? now - d : 0;
        } else {
            now += rnd() % 1500u;
        }
        video_codec_policy_input_t in;
        memset(&in, 0, sizeof(in));
        in.now_ms = now;
        in.source_supported_codecs = chance(95) ? (chance(50) ? 2u : 0xFFFFFFFFu) : (chance(50) ? 0u : 1u);
        in.client_count = chance(8) ? 0 : (chance(70) ? 1 : (rnd() % 4));
        in.h264_client_count = chance(85) ? in.client_count : (chance(80) ? (in.client_count ? in.client_count - 1 : 0) : rnd() % 5);
        in.locked = chance(96);
        in.width = chance(92) ? 1920 : (chance(50) ? 1921 : (chance(50) ? 0 : 1280));
        in.height = chance(92) ? 1080 : (chance(50) ? 1081 : (chance(50) ? 0 : 720));
        if (!in.locked && chance(50)) { in.width = 0; in.height = 0; }

        int got = (int)video_codec_policy_update(&p, &in);
        int want = model_step(&m, allowed, dwell, &in);
        TEST_ASSERT_EQUAL_INT_MESSAGE(want, got, msg);
        TEST_ASSERT_EQUAL_INT_MESSAGE(got, (int)video_codec_policy_get_codec(&p), msg);

        /* 不変条件（モデルに依らない） */
        bool hard = allowed && (in.source_supported_codecs & 2u) && in.locked && in.width >= 1 && in.width <= 1920 &&
                    in.height >= 1 && in.height <= 1080 && in.h264_client_count <= in.client_count;
        if (!allowed) TEST_ASSERT_EQUAL_INT_MESSAGE(0, got, msg);
        if (!hard) TEST_ASSERT_EQUAL_INT_MESSAGE(0, got, msg);                 /* 即時の降格 */
        if (prev == 1 && in.client_count >= 1 && in.h264_client_count != in.client_count)
            TEST_ASSERT_EQUAL_INT_MESSAGE(0, got, msg);                        /* 申告なしのクライアント */
        if (prev == 1 && hard && in.client_count == 0) TEST_ASSERT_EQUAL_INT_MESSAGE(1, got, msg); /* 0 個は変えない */
        if (prev == 0 && got == 1) {                                           /* 昇格の不変条件 */
            TEST_ASSERT_TRUE_MESSAGE(hard, msg);
            TEST_ASSERT_TRUE_MESSAGE(in.client_count >= 1 && in.h264_client_count == in.client_count, msg);
            TEST_ASSERT_TRUE_MESSAGE(m.streak == false || m.streak_start <= in.now_ms, msg);
            (*upgrades_seen)++;
        }
        if (prev == 1 && got == 0) (*downgrades_seen)++;
        prev = got;
    }
    video_codec_policy_stats_t s;
    video_codec_policy_get_stats(&p, &s);
    TEST_ASSERT_TRUE(s.upgrades >= s.downgrades);
    TEST_ASSERT_TRUE(s.upgrades - s.downgrades <= 1u);
    TEST_ASSERT_EQUAL_UINT32(s.upgrades - s.downgrades, (uint32_t)prev);
}

void test_fuzz_against_reference_model_and_invariants(void) {
    uint32_t up = 0, down = 0;
    for (uint64_t seed = 1; seed <= 600; seed++) fuzz_one(seed, 400, &up, &down);
    /* ファズが、昇格と降格を実際に起こしていること（空振りで通っていないこと） */
    TEST_ASSERT_TRUE_MESSAGE(up > 300, "fuzz made too few upgrades");
    TEST_ASSERT_TRUE_MESSAGE(down > 300, "fuzz made too few downgrades");
}

void test_fuzz_never_allowed_is_always_mjpeg(void) {
    for (uint64_t seed = 1; seed <= 200; seed++) {
        g_rng = seed * 0x2545F4914F6CDD1Dull + 7;
        video_codec_policy_t p;
        video_codec_policy_config_t c = cfg(false, rnd() % 3 == 0 ? 0 : 5000);
        video_codec_policy_init(&p, &c);
        uint64_t now = 0;
        for (int i = 0; i < 300; i++) {
            now += rnd() % 7000u;
            video_codec_policy_input_t in = good(now);
            in.client_count = rnd() % 3;
            in.h264_client_count = in.client_count;
            TEST_ASSERT_EQUAL_INT(MJPEG, tick(&p, &in));
        }
    }
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_config_default_values);
    RUN_TEST(test_init_is_mjpeg_with_zero_stats_and_keeps_config);
    RUN_TEST(test_init_with_null_config_is_default_and_never_h264);
    RUN_TEST(test_not_allowed_is_always_mjpeg_for_any_input);
    RUN_TEST(test_each_condition_alone_blocks_the_upgrade);
    RUN_TEST(test_zero_clients_blocks_the_upgrade);
    RUN_TEST(test_capability_mask_only_the_h264_bit_counts);
    RUN_TEST(test_upgrade_waits_exactly_dwell_ms_ticking_every_second);
    RUN_TEST(test_dwell_boundary_minus_one_exact_plus_one);
    RUN_TEST(test_dwell_zero_upgrades_on_the_first_satisfied_tick);
    RUN_TEST(test_dwell_one_boundary);
    RUN_TEST(test_dwell_counts_from_the_first_satisfied_tick_not_from_init);
    RUN_TEST(test_dwell_is_not_extended_by_more_satisfied_ticks);
    RUN_TEST(test_any_single_break_restarts_the_dwell);
    RUN_TEST(test_break_at_the_very_tick_the_dwell_would_complete_prevents_the_upgrade);
    RUN_TEST(test_second_client_without_declaration_joining_then_leaving_restarts_the_dwell);
    RUN_TEST(test_two_declared_clients_upgrade);
    RUN_TEST(test_every_hard_condition_downgrades_at_once_from_h264);
    RUN_TEST(test_hard_conditions_downgrade_even_with_zero_clients);
    RUN_TEST(test_undeclared_client_downgrades_at_once);
    RUN_TEST(test_one_of_two_clients_undeclared_downgrades_at_once);
    RUN_TEST(test_h264_stays_while_all_conditions_hold);
    RUN_TEST(test_h264_stays_with_a_second_declared_client_joining);
    RUN_TEST(test_after_a_downgrade_the_dwell_starts_over);
    RUN_TEST(test_stale_dwell_is_not_reused_after_upgrade_then_downgrade);
    RUN_TEST(test_zero_clients_keeps_h264);
    RUN_TEST(test_zero_clients_keeps_mjpeg);
    RUN_TEST(test_h264_after_zero_clients_is_judged_again_by_the_next_connection);
    RUN_TEST(test_zero_clients_discards_the_dwell);
    RUN_TEST(test_resolution_limit_boundaries_with_defaults);
    RUN_TEST(test_resolution_limit_follows_the_config);
    RUN_TEST(test_zero_limit_allows_no_resolution);
    RUN_TEST(test_resolution_change_over_the_limit_downgrades_and_back_within_waits);
    RUN_TEST(test_locked_false_ignores_the_reported_resolution);
    RUN_TEST(test_clock_regression_restarts_the_dwell);
    RUN_TEST(test_clock_regression_below_the_dwell_start_does_not_wrap_around);
    RUN_TEST(test_clock_regression_to_zero_at_the_64bit_limit);
    RUN_TEST(test_clock_regression_while_broken_does_not_affect_the_next_dwell);
    RUN_TEST(test_clock_regression_in_h264_does_not_downgrade);
    RUN_TEST(test_equal_now_ms_is_not_a_regression);
    RUN_TEST(test_regression_by_one_ms_restarts_the_dwell);
    RUN_TEST(test_large_now_values_do_not_overflow);
    RUN_TEST(test_inconsistent_counts_never_upgrade);
    RUN_TEST(test_inconsistent_counts_downgrade_h264_even_with_zero_clients);
    RUN_TEST(test_inconsistent_input_discards_the_dwell);
    RUN_TEST(test_null_arguments_do_not_crash_and_are_safe);
    RUN_TEST(test_null_input_downgrades_h264_and_discards_the_dwell);
    RUN_TEST(test_allowed_switched_off_downgrades_even_with_zero_clients);
    RUN_TEST(test_allowed_switched_on_again_waits_the_dwell);
    RUN_TEST(test_allowed_off_during_the_dwell_restarts_it);
    RUN_TEST(test_stats_count_each_transition_once);
    RUN_TEST(test_fuzz_against_reference_model_and_invariants);
    RUN_TEST(test_fuzz_never_allowed_is_always_mjpeg);
    return UNITY_END();
}
