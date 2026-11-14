#include "unity.h"
#include "http_guard.h"
#include <string.h>
#include <stdio.h>

void setUp(void) {}
void tearDown(void) {}

// 状態の応答(GET /api/v1/auth/status)。video_port は docs/VIDEO_TRANSPORT.md 5 章。
// 期待する文字列は、手で書いたもの(実装と同じ書式文字列から作らない)。

static void expect(auth_state_e st, bool auth, uint16_t port, const char *want) {
    char buf[128];
    memset(buf, 'X', sizeof(buf));
    TEST_ASSERT_EQUAL_INT(0, http_guard_format_status(st, auth, port, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING(want, buf);
}

void test_status_without_video_port_is_backward_compatible(void) {
    expect(AUTH_STATE_NEEDS_SETUP, false, 0, "{\"state\":\"needs_setup\",\"authenticated\":false}");
    expect(AUTH_STATE_READY, false, 0, "{\"state\":\"ready\",\"authenticated\":false}");
    expect(AUTH_STATE_READY, true, 0, "{\"state\":\"ready\",\"authenticated\":true}");
    expect(AUTH_STATE_NEEDS_SETUP, true, 0, "{\"state\":\"needs_setup\",\"authenticated\":true}");
}

void test_status_zero_video_port_has_no_video_port_key(void) {
    char buf[128];
    TEST_ASSERT_EQUAL_INT(0, http_guard_format_status(AUTH_STATE_READY, true, 0, buf, sizeof(buf)));
    TEST_ASSERT_NULL(strstr(buf, "video_port"));
}

void test_status_with_video_port(void) {
    expect(AUTH_STATE_READY, true, 8444, "{\"state\":\"ready\",\"authenticated\":true,\"video_port\":8444}");
    expect(AUTH_STATE_READY, false, 8444, "{\"state\":\"ready\",\"authenticated\":false,\"video_port\":8444}");
    expect(AUTH_STATE_NEEDS_SETUP, false, 8444, "{\"state\":\"needs_setup\",\"authenticated\":false,\"video_port\":8444}");
}

void test_status_video_port_boundaries(void) {
    expect(AUTH_STATE_READY, true, 1, "{\"state\":\"ready\",\"authenticated\":true,\"video_port\":1}");
    expect(AUTH_STATE_READY, true, 9, "{\"state\":\"ready\",\"authenticated\":true,\"video_port\":9}");
    expect(AUTH_STATE_READY, true, 10, "{\"state\":\"ready\",\"authenticated\":true,\"video_port\":10}");
    expect(AUTH_STATE_READY, true, 443, "{\"state\":\"ready\",\"authenticated\":true,\"video_port\":443}");
    expect(AUTH_STATE_READY, true, 32767, "{\"state\":\"ready\",\"authenticated\":true,\"video_port\":32767}");
    expect(AUTH_STATE_READY, true, 32768, "{\"state\":\"ready\",\"authenticated\":true,\"video_port\":32768}");
    expect(AUTH_STATE_READY, true, 65534, "{\"state\":\"ready\",\"authenticated\":true,\"video_port\":65534}");
    // 65535 は符号なしで出る(負の数にならない)
    expect(AUTH_STATE_READY, true, 65535, "{\"state\":\"ready\",\"authenticated\":true,\"video_port\":65535}");
}

void test_status_every_port_value_is_an_exact_integer(void) {
    // 全ポート値で、数字が先頭ゼロなしの 10 進数で、その直後が '}' であること。
    for (unsigned p = 1; p <= 65535; p++) {
        char buf[128];
        char want[16];
        TEST_ASSERT_EQUAL_INT(0, http_guard_format_status(AUTH_STATE_READY, true, (uint16_t)p, buf, sizeof(buf)));
        const char *k = strstr(buf, "\"video_port\":");
        TEST_ASSERT_NOT_NULL(k);
        const char *d = k + strlen("\"video_port\":");
        TEST_ASSERT_TRUE(*d >= '1' && *d <= '9');
        snprintf(want, sizeof(want), "%u}", p);
        TEST_ASSERT_EQUAL_STRING(want, d);
    }
}

void test_status_buffer_exact_fit_and_one_short(void) {
    const char *w0 = "{\"state\":\"ready\",\"authenticated\":false}";
    size_t n0 = strlen(w0);
    char buf[128];
    TEST_ASSERT_EQUAL_INT(0, http_guard_format_status(AUTH_STATE_READY, false, 0, buf, n0 + 1));
    TEST_ASSERT_EQUAL_STRING(w0, buf);
    TEST_ASSERT_EQUAL_INT(-1, http_guard_format_status(AUTH_STATE_READY, false, 0, buf, n0));

    const char *w1 = "{\"state\":\"ready\",\"authenticated\":false,\"video_port\":65535}";
    size_t n1 = strlen(w1);
    TEST_ASSERT_EQUAL_INT(0, http_guard_format_status(AUTH_STATE_READY, false, 65535, buf, n1 + 1));
    TEST_ASSERT_EQUAL_STRING(w1, buf);
    TEST_ASSERT_EQUAL_INT(-1, http_guard_format_status(AUTH_STATE_READY, false, 65535, buf, n1));

    const char *w2 = "{\"state\":\"needs_setup\",\"authenticated\":true,\"video_port\":1}";
    size_t n2 = strlen(w2);
    TEST_ASSERT_EQUAL_INT(0, http_guard_format_status(AUTH_STATE_NEEDS_SETUP, true, 1, buf, n2 + 1));
    TEST_ASSERT_EQUAL_STRING(w2, buf);
    TEST_ASSERT_EQUAL_INT(-1, http_guard_format_status(AUTH_STATE_NEEDS_SETUP, true, 1, buf, n2));
}

void test_status_buffer_too_small_never_writes_past_the_end(void) {
    // out_len ごとに、境界の後ろに番兵を置いて、書き込みが範囲内であることを確認する(ASan も見る)。
    for (size_t len = 1; len < 70; len++) {
        char area[96];
        memset(area, 0x5A, sizeof(area));
        int r = http_guard_format_status(AUTH_STATE_NEEDS_SETUP, true, 65535, area, len);
        TEST_ASSERT_TRUE(r == 0 || r == -1);
        for (size_t i = len; i < sizeof(area); i++) TEST_ASSERT_EQUAL_HEX8(0x5A, (unsigned char)area[i]);
        if (r == -1) {
            // 切り詰めても NUL 終端されている(snprintf の規則)
            TEST_ASSERT_TRUE(memchr(area, '\0', len) != NULL);
        }
    }
}

void test_status_null_and_zero_length(void) {
    char buf[8];
    TEST_ASSERT_EQUAL_INT(-1, http_guard_format_status(AUTH_STATE_READY, true, 8444, NULL, 128));
    TEST_ASSERT_EQUAL_INT(-1, http_guard_format_status(AUTH_STATE_READY, true, 8444, buf, 0));
    TEST_ASSERT_EQUAL_INT(-1, http_guard_format_status(AUTH_STATE_READY, true, 0, NULL, 128));
    TEST_ASSERT_EQUAL_INT(-1, http_guard_format_status(AUTH_STATE_READY, true, 0, buf, 0));
}

void test_status_fits_the_caller_buffer(void) {
    // web_server.c の呼び出し側のバッファは 256 バイト。最長の応答が収まること。
    char buf[256];
    TEST_ASSERT_EQUAL_INT(0, http_guard_format_status(AUTH_STATE_NEEDS_SETUP, false, 65535, buf, sizeof(buf)));
    TEST_ASSERT_TRUE(strlen(buf) < 100);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_status_without_video_port_is_backward_compatible);
    RUN_TEST(test_status_zero_video_port_has_no_video_port_key);
    RUN_TEST(test_status_with_video_port);
    RUN_TEST(test_status_video_port_boundaries);
    RUN_TEST(test_status_every_port_value_is_an_exact_integer);
    RUN_TEST(test_status_buffer_exact_fit_and_one_short);
    RUN_TEST(test_status_buffer_too_small_never_writes_past_the_end);
    RUN_TEST(test_status_null_and_zero_length);
    RUN_TEST(test_status_fits_the_caller_buffer);
    return UNITY_END();
}
