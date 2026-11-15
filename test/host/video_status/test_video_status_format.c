/* video_status_format_json（C-6b。docs/VIDEO_TRANSPORT.md 8 章、docs/WEB_API.md 5 章）:
 * 固定の書式（完全一致）、3 つの状態、解像度の境界、補正（範囲外の state、LOCKED で 0、LOCKED 以外の解像度）、
 * バッファの大きさの境界、NULL、部分的な文字列を残さないこと、video_status_get の補正との一致 */
#include "unity.h"
#include "video_status.h"
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

void setUp(void) { video_status_set_provider(NULL, NULL); }
void tearDown(void) { video_status_set_provider(NULL, NULL); }

static video_status_t mk(kvm_video_sync_state_t s, uint32_t w, uint32_t h) {
    video_status_t v;
    v.state = s;
    v.width = w;
    v.height = h;
    return v;
}

/* 文字列と戻り値の両方を確かめる */
static void expect_json(video_status_t st, const char *want) {
    char buf[VIDEO_STATUS_JSON_MAX];
    memset(buf, 0x55, sizeof(buf));
    int n = video_status_format_json(&st, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_INT((int)strlen(want), n);
    TEST_ASSERT_EQUAL_STRING(want, buf);
    TEST_ASSERT_EQUAL_INT(n, (int)strlen(buf));
}

/* ---- 固定の書式 ---- */

void test_format_three_states_exact_strings(void) {
    expect_json(mk(KVM_VIDEO_SYNC_LOCKED, 1280, 720), "{\"t\":\"video\",\"state\":\"locked\",\"w\":1280,\"h\":720}");
    expect_json(mk(KVM_VIDEO_SYNC_NO_SIGNAL, 0, 0), "{\"t\":\"video\",\"state\":\"no_signal\",\"w\":0,\"h\":0}");
    expect_json(mk(KVM_VIDEO_SYNC_UNKNOWN, 0, 0), "{\"t\":\"video\",\"state\":\"unknown\",\"w\":0,\"h\":0}");
}

void test_format_is_fixed_key_order_without_whitespace(void) {
    char buf[VIDEO_STATUS_JSON_MAX];
    video_status_t st = mk(KVM_VIDEO_SYNC_LOCKED, 1920, 1080);
    int n = video_status_format_json(&st, buf, sizeof(buf));
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_EQUAL_STRING("{\"t\":\"video\",\"state\":\"locked\",\"w\":1920,\"h\":1080}", buf);
    for (int i = 0; i < n; i++) {
        TEST_ASSERT_TRUE(buf[i] != ' ' && buf[i] != '\n' && buf[i] != '\t' && buf[i] != '\r');
    }
    /* キーの順序: t, state, w, h */
    const char *t = strstr(buf, "\"t\":");
    const char *s = strstr(buf, "\"state\":");
    const char *w = strstr(buf, "\"w\":");
    const char *h = strstr(buf, "\"h\":");
    TEST_ASSERT_NOT_NULL(t);
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_NOT_NULL(w);
    TEST_ASSERT_NOT_NULL(h);
    TEST_ASSERT_TRUE(t < s && s < w && w < h);
    TEST_ASSERT_EQUAL_CHAR('{', buf[0]);
    TEST_ASSERT_EQUAL_CHAR('}', buf[n - 1]);
}

void test_format_locked_common_resolutions(void) {
    expect_json(mk(KVM_VIDEO_SYNC_LOCKED, 1920, 1080), "{\"t\":\"video\",\"state\":\"locked\",\"w\":1920,\"h\":1080}");
    expect_json(mk(KVM_VIDEO_SYNC_LOCKED, 640, 480), "{\"t\":\"video\",\"state\":\"locked\",\"w\":640,\"h\":480}");
    /* w と h を取り違えない */
    expect_json(mk(KVM_VIDEO_SYNC_LOCKED, 1, 2), "{\"t\":\"video\",\"state\":\"locked\",\"w\":1,\"h\":2}");
    expect_json(mk(KVM_VIDEO_SYNC_LOCKED, 2, 1), "{\"t\":\"video\",\"state\":\"locked\",\"w\":2,\"h\":1}");
}

/* ---- 解像度の境界 ---- */

void test_format_resolution_boundaries(void) {
    expect_json(mk(KVM_VIDEO_SYNC_LOCKED, 1, 1), "{\"t\":\"video\",\"state\":\"locked\",\"w\":1,\"h\":1}");
    expect_json(mk(KVM_VIDEO_SYNC_LOCKED, 65535, 65535), "{\"t\":\"video\",\"state\":\"locked\",\"w\":65535,\"h\":65535}");
    expect_json(mk(KVM_VIDEO_SYNC_LOCKED, 65536, 65536), "{\"t\":\"video\",\"state\":\"locked\",\"w\":65536,\"h\":65536}");
    expect_json(mk(KVM_VIDEO_SYNC_LOCKED, 9, 10), "{\"t\":\"video\",\"state\":\"locked\",\"w\":9,\"h\":10}");
    expect_json(mk(KVM_VIDEO_SYNC_LOCKED, 99999, 100000), "{\"t\":\"video\",\"state\":\"locked\",\"w\":99999,\"h\":100000}");
    expect_json(mk(KVM_VIDEO_SYNC_LOCKED, UINT32_MAX, UINT32_MAX),
                "{\"t\":\"video\",\"state\":\"locked\",\"w\":4294967295,\"h\":4294967295}");
    expect_json(mk(KVM_VIDEO_SYNC_LOCKED, UINT32_MAX, 1), "{\"t\":\"video\",\"state\":\"locked\",\"w\":4294967295,\"h\":1}");
    expect_json(mk(KVM_VIDEO_SYNC_LOCKED, 1, UINT32_MAX), "{\"t\":\"video\",\"state\":\"locked\",\"w\":1,\"h\":4294967295}");
    expect_json(mk(KVM_VIDEO_SYNC_LOCKED, 2147483648u, 2147483647u),
                "{\"t\":\"video\",\"state\":\"locked\",\"w\":2147483648,\"h\":2147483647}");
}

void test_format_max_length_is_60_and_within_128(void) {
    char buf[VIDEO_STATUS_JSON_MAX];
    video_status_t st = mk(KVM_VIDEO_SYNC_LOCKED, UINT32_MAX, UINT32_MAX);
    int n = video_status_format_json(&st, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_INT(60, n);
    TEST_ASSERT_TRUE(VIDEO_STATUS_JSON_MAX <= 128);
    TEST_ASSERT_TRUE(n + 1 <= VIDEO_STATUS_JSON_MAX);
    /* 契約の上限 (128 バイト以下) を、どの状態でも超えない */
    for (int s = -3; s <= 6; s++) {
        st = mk((kvm_video_sync_state_t)s, UINT32_MAX, UINT32_MAX);
        n = video_status_format_json(&st, buf, sizeof(buf));
        TEST_ASSERT_TRUE(n > 0 && n <= 60);
    }
}

/* ---- LOCKED 以外は w = h = 0 ---- */

void test_format_non_locked_zeroes_resolution(void) {
    expect_json(mk(KVM_VIDEO_SYNC_NO_SIGNAL, 1920, 1080), "{\"t\":\"video\",\"state\":\"no_signal\",\"w\":0,\"h\":0}");
    expect_json(mk(KVM_VIDEO_SYNC_NO_SIGNAL, 1, 0), "{\"t\":\"video\",\"state\":\"no_signal\",\"w\":0,\"h\":0}");
    expect_json(mk(KVM_VIDEO_SYNC_NO_SIGNAL, 0, 1), "{\"t\":\"video\",\"state\":\"no_signal\",\"w\":0,\"h\":0}");
    expect_json(mk(KVM_VIDEO_SYNC_UNKNOWN, 1920, 1080), "{\"t\":\"video\",\"state\":\"unknown\",\"w\":0,\"h\":0}");
    expect_json(mk(KVM_VIDEO_SYNC_UNKNOWN, 1, 0), "{\"t\":\"video\",\"state\":\"unknown\",\"w\":0,\"h\":0}");
    expect_json(mk(KVM_VIDEO_SYNC_UNKNOWN, 0, 1), "{\"t\":\"video\",\"state\":\"unknown\",\"w\":0,\"h\":0}");
    expect_json(mk(KVM_VIDEO_SYNC_UNKNOWN, UINT32_MAX, UINT32_MAX), "{\"t\":\"video\",\"state\":\"unknown\",\"w\":0,\"h\":0}");
}

/* ---- 範囲外の state は unknown ---- */

void test_format_out_of_range_state_is_unknown(void) {
    const int bad[] = {3, 4, 42, 255, 256, 65535, INT_MAX, -1, -2, INT_MIN};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        expect_json(mk((kvm_video_sync_state_t)bad[i], 0, 0), "{\"t\":\"video\",\"state\":\"unknown\",\"w\":0,\"h\":0}");
        /* 解像度が付いていても 0 にする (LOCKED と間違えない) */
        expect_json(mk((kvm_video_sync_state_t)bad[i], 1280, 720), "{\"t\":\"video\",\"state\":\"unknown\",\"w\":0,\"h\":0}");
    }
}

/* ---- LOCKED で w または h が 0 は unknown ---- */

void test_format_locked_with_zero_dimension_is_unknown(void) {
    const char *unk = "{\"t\":\"video\",\"state\":\"unknown\",\"w\":0,\"h\":0}";
    expect_json(mk(KVM_VIDEO_SYNC_LOCKED, 0, 0), unk);
    expect_json(mk(KVM_VIDEO_SYNC_LOCKED, 0, 720), unk);
    expect_json(mk(KVM_VIDEO_SYNC_LOCKED, 1280, 0), unk);
    expect_json(mk(KVM_VIDEO_SYNC_LOCKED, UINT32_MAX, 0), unk);
    expect_json(mk(KVM_VIDEO_SYNC_LOCKED, 0, UINT32_MAX), unk);
    /* 0 でなければ locked のまま (境界) */
    expect_json(mk(KVM_VIDEO_SYNC_LOCKED, 1, 1), "{\"t\":\"video\",\"state\":\"locked\",\"w\":1,\"h\":1}");
}

/* ---- バッファの大きさの境界 ---- */

void test_format_exact_capacity_succeeds_and_one_short_fails(void) {
    video_status_t st = mk(KVM_VIDEO_SYNC_LOCKED, 1280, 720);
    const char *want = "{\"t\":\"video\",\"state\":\"locked\",\"w\":1280,\"h\":720}";
    size_t len = strlen(want);
    char buf[VIDEO_STATUS_JSON_MAX + 8];

    /* ちょうど (文字数 + NUL) */
    memset(buf, 0x55, sizeof(buf));
    TEST_ASSERT_EQUAL_INT((int)len, video_status_format_json(&st, buf, len + 1));
    TEST_ASSERT_EQUAL_STRING(want, buf);
    TEST_ASSERT_EQUAL_UINT8(0x55, (uint8_t)buf[len + 1]); /* NUL の先は書かない */

    /* 1 足りない (NUL が入らない): -1 で、部分的な文字列を残さない */
    memset(buf, 0x55, sizeof(buf));
    TEST_ASSERT_EQUAL_INT(-1, video_status_format_json(&st, buf, len));
    TEST_ASSERT_EQUAL_STRING("", buf);
    TEST_ASSERT_EQUAL_UINT8(0x55, (uint8_t)buf[1]); /* それ以外には触れない */
    TEST_ASSERT_EQUAL_UINT8(0x55, (uint8_t)buf[len]);

    /* 余裕がある */
    memset(buf, 0x55, sizeof(buf));
    TEST_ASSERT_EQUAL_INT((int)len, video_status_format_json(&st, buf, len + 2));
    TEST_ASSERT_EQUAL_STRING(want, buf);
    TEST_ASSERT_EQUAL_UINT8(0x55, (uint8_t)buf[len + 2]);
}

void test_format_exact_capacity_for_each_state_and_max_digits(void) {
    video_status_t cases[] = {
        mk(KVM_VIDEO_SYNC_LOCKED, UINT32_MAX, UINT32_MAX),
        mk(KVM_VIDEO_SYNC_LOCKED, 1, 1),
        mk(KVM_VIDEO_SYNC_NO_SIGNAL, 0, 0),
        mk(KVM_VIDEO_SYNC_UNKNOWN, 0, 0),
    };
    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        char full[VIDEO_STATUS_JSON_MAX];
        int len = video_status_format_json(&cases[c], full, sizeof(full));
        TEST_ASSERT_TRUE(len > 0);
        char buf[VIDEO_STATUS_JSON_MAX];
        memset(buf, 0x55, sizeof(buf));
        TEST_ASSERT_EQUAL_INT(len, video_status_format_json(&cases[c], buf, (size_t)len + 1));
        TEST_ASSERT_EQUAL_STRING(full, buf);
        memset(buf, 0x55, sizeof(buf));
        TEST_ASSERT_EQUAL_INT(-1, video_status_format_json(&cases[c], buf, (size_t)len));
        TEST_ASSERT_EQUAL_STRING("", buf);
        /* 0 .. len までのどの cap でも、失敗して空の文字列 (cap = 0 は触らない) */
        for (size_t cap = 1; cap <= (size_t)len; cap++) {
            memset(buf, 0x55, sizeof(buf));
            TEST_ASSERT_EQUAL_INT(-1, video_status_format_json(&cases[c], buf, cap));
            TEST_ASSERT_EQUAL_UINT8(0, (uint8_t)buf[0]);
            TEST_ASSERT_EQUAL_UINT8(0x55, (uint8_t)buf[cap < sizeof(buf) ? cap : sizeof(buf) - 1]);
        }
    }
}

void test_format_cap_zero_and_one(void) {
    video_status_t st = mk(KVM_VIDEO_SYNC_NO_SIGNAL, 0, 0);
    char buf[4];
    memset(buf, 0x55, sizeof(buf));
    TEST_ASSERT_EQUAL_INT(-1, video_status_format_json(&st, buf, 0));
    TEST_ASSERT_EQUAL_UINT8(0x55, (uint8_t)buf[0]); /* cap == 0 は 1 バイトも書かない */
    TEST_ASSERT_EQUAL_UINT8(0x55, (uint8_t)buf[1]);
    memset(buf, 0x55, sizeof(buf));
    TEST_ASSERT_EQUAL_INT(-1, video_status_format_json(&st, buf, 1));
    TEST_ASSERT_EQUAL_UINT8(0, (uint8_t)buf[0]);
    TEST_ASSERT_EQUAL_UINT8(0x55, (uint8_t)buf[1]);
}

void test_format_large_capacity_is_fine(void) {
    video_status_t st = mk(KVM_VIDEO_SYNC_LOCKED, 800, 600);
    char buf[512];
    memset(buf, 0x55, sizeof(buf));
    int n = video_status_format_json(&st, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_INT(46, n);
    TEST_ASSERT_EQUAL_STRING("{\"t\":\"video\",\"state\":\"locked\",\"w\":800,\"h\":600}", buf);
    TEST_ASSERT_EQUAL_UINT8(0x55, (uint8_t)buf[n + 1]);
    TEST_ASSERT_EQUAL_UINT8(0x55, (uint8_t)buf[sizeof(buf) - 1]);
}

/* ---- NULL ---- */

void test_format_null_arguments(void) {
    char buf[VIDEO_STATUS_JSON_MAX];
    video_status_t st = mk(KVM_VIDEO_SYNC_LOCKED, 1280, 720);
    TEST_ASSERT_EQUAL_INT(-1, video_status_format_json(&st, NULL, sizeof(buf)));
    TEST_ASSERT_EQUAL_INT(-1, video_status_format_json(&st, NULL, 0));
    memset(buf, 0x55, sizeof(buf));
    TEST_ASSERT_EQUAL_INT(-1, video_status_format_json(NULL, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("", buf); /* 部分的な文字列を残さない */
    TEST_ASSERT_EQUAL_INT(-1, video_status_format_json(NULL, NULL, 0));
}

void test_format_does_not_modify_input(void) {
    video_status_t st = mk((kvm_video_sync_state_t)77, 123, 456);
    char buf[VIDEO_STATUS_JSON_MAX];
    video_status_format_json(&st, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_INT(77, st.state);
    TEST_ASSERT_EQUAL_UINT32(123, st.width);
    TEST_ASSERT_EQUAL_UINT32(456, st.height);
}

/* ---- video_status_get の補正との一致 ---- */

void test_format_matches_video_status_get_correction(void) {
    const int states[] = {-1, 0, 1, 2, 3, 99};
    const uint32_t dims[] = {0, 1, 720, 65535, UINT32_MAX};
    for (size_t a = 0; a < sizeof(states) / sizeof(states[0]); a++) {
        for (size_t w = 0; w < sizeof(dims) / sizeof(dims[0]); w++) {
            for (size_t h = 0; h < sizeof(dims) / sizeof(dims[0]); h++) {
                video_status_fake_t f;
                video_status_fake_init(&f);
                video_status_fake_set(&f, (kvm_video_sync_state_t)states[a], dims[w], dims[h]);
                video_status_set_provider(video_status_fake_provider, &f);
                video_status_t got;
                video_status_get(&got); /* 補正済み */
                video_status_set_provider(NULL, NULL);

                char raw_json[VIDEO_STATUS_JSON_MAX], got_json[VIDEO_STATUS_JSON_MAX];
                video_status_t raw = mk((kvm_video_sync_state_t)states[a], dims[w], dims[h]);
                TEST_ASSERT_TRUE(video_status_format_json(&raw, raw_json, sizeof(raw_json)) > 0);
                TEST_ASSERT_TRUE(video_status_format_json(&got, got_json, sizeof(got_json)) > 0);
                TEST_ASSERT_EQUAL_STRING(got_json, raw_json); /* 未補正の値を渡しても、補正済みと同じ文字列 */
            }
        }
    }
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_format_three_states_exact_strings);
    RUN_TEST(test_format_is_fixed_key_order_without_whitespace);
    RUN_TEST(test_format_locked_common_resolutions);
    RUN_TEST(test_format_resolution_boundaries);
    RUN_TEST(test_format_max_length_is_60_and_within_128);
    RUN_TEST(test_format_non_locked_zeroes_resolution);
    RUN_TEST(test_format_out_of_range_state_is_unknown);
    RUN_TEST(test_format_locked_with_zero_dimension_is_unknown);
    RUN_TEST(test_format_exact_capacity_succeeds_and_one_short_fails);
    RUN_TEST(test_format_exact_capacity_for_each_state_and_max_digits);
    RUN_TEST(test_format_cap_zero_and_one);
    RUN_TEST(test_format_large_capacity_is_fine);
    RUN_TEST(test_format_null_arguments);
    RUN_TEST(test_format_does_not_modify_input);
    RUN_TEST(test_format_matches_video_status_get_correction);
    return UNITY_END();
}
