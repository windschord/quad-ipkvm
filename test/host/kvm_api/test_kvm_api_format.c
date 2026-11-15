#include "unity.h"
#include "kvm_api.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* 状態の JSON（docs/WEB_API.md 7.1）の生成 */

void setUp(void) {}
void tearDown(void) {}

static kvm_ctrl_status_t base_status(void) {
    kvm_ctrl_status_t s;
    memset(&s, 0, sizeof(s));
    s.state = KVM_CTRL_STATE_IDLE;
    s.active_channel = 2;
    s.video_sync = KVM_VIDEO_SYNC_LOCKED;
    return s;
}

static void expect_json(const kvm_ctrl_status_t *s, const char *json) {
    char out[KVM_API_RESPONSE_MAX];
    size_t len = 12345;
    TEST_ASSERT_EQUAL_INT(0, kvm_api_format_status(s, out, sizeof(out), &len));
    TEST_ASSERT_EQUAL_STRING(json, out);
    TEST_ASSERT_EQUAL_UINT((unsigned)strlen(json), (unsigned)len);
}

void test_doc_example_exact(void) {
    /* docs/WEB_API.md 7.1 の例 */
    kvm_ctrl_status_t s = base_status();
    s.has_last_op = true;
    s.last_op.op_id = 7;
    s.last_op.kind = KVM_CTRL_OP_POWER_PRESS;
    s.last_op.channel = 2;
    s.last_op.result = KVM_CTRL_OP_COMPLETED;
    expect_json(&s,
        "{\"state\":\"idle\",\"active_channel\":2,\"video_sync\":\"locked\",\"atx_locked\":false,\"atx_anomaly\":false,"
        "\"fault\":false,\"cooldown_ms\":0,\"last_op\":{\"op_id\":7,\"kind\":\"power_press\",\"channel\":2,\"result\":\"completed\"}}");
}

void test_unknown_state_has_null_channel_and_no_last_op(void) {
    kvm_ctrl_status_t s = base_status();
    s.state = KVM_CTRL_STATE_UNKNOWN;
    s.active_channel = 0;
    s.video_sync = KVM_VIDEO_SYNC_UNKNOWN;
    s.atx_locked = true;
    expect_json(&s,
        "{\"state\":\"unknown\",\"active_channel\":null,\"video_sync\":\"unknown\",\"atx_locked\":true,\"atx_anomaly\":false,"
        "\"fault\":false,\"cooldown_ms\":0,\"last_op\":null}");
}

void test_unknown_state_hides_a_stale_channel(void) {
    /* state が unknown なら、active_channel が 1..4 でも null にする */
    kvm_ctrl_status_t s = base_status();
    s.state = KVM_CTRL_STATE_UNKNOWN;
    s.active_channel = 3;
    char out[KVM_API_RESPONSE_MAX];
    TEST_ASSERT_EQUAL_INT(0, kvm_api_format_status(&s, out, sizeof(out), NULL));
    TEST_ASSERT_NOT_NULL(strstr(out, "\"active_channel\":null,"));
}

void test_active_channel_out_of_range_is_null(void) {
    char out[KVM_API_RESPONSE_MAX];
    kvm_ctrl_status_t s = base_status();
    s.active_channel = 0;
    TEST_ASSERT_EQUAL_INT(0, kvm_api_format_status(&s, out, sizeof(out), NULL));
    TEST_ASSERT_NOT_NULL(strstr(out, "\"active_channel\":null,"));
    s.active_channel = 5;
    TEST_ASSERT_EQUAL_INT(0, kvm_api_format_status(&s, out, sizeof(out), NULL));
    TEST_ASSERT_NOT_NULL(strstr(out, "\"active_channel\":null,"));
    s.active_channel = 255;
    TEST_ASSERT_EQUAL_INT(0, kvm_api_format_status(&s, out, sizeof(out), NULL));
    TEST_ASSERT_NOT_NULL(strstr(out, "\"active_channel\":null,"));
}

void test_each_active_channel(void) {
    for (uint8_t ch = 1; ch <= 4; ch++) {
        kvm_ctrl_status_t s = base_status();
        s.active_channel = ch;
        char out[KVM_API_RESPONSE_MAX];
        char want[40];
        snprintf(want, sizeof(want), "\"active_channel\":%u,", (unsigned)ch);
        TEST_ASSERT_EQUAL_INT(0, kvm_api_format_status(&s, out, sizeof(out), NULL));
        TEST_ASSERT_NOT_NULL(strstr(out, want));
    }
}

void test_state_names(void) {
    static const struct { kvm_ctrl_state_t s; const char *name; } T[] = {
        {KVM_CTRL_STATE_UNKNOWN, "unknown"}, {KVM_CTRL_STATE_IDLE, "idle"}, {KVM_CTRL_STATE_SWITCHING, "switching"},
        {KVM_CTRL_STATE_TRACKING, "tracking"}, {KVM_CTRL_STATE_ATX, "atx"},
    };
    for (size_t i = 0; i < sizeof(T) / sizeof(T[0]); i++) {
        kvm_ctrl_status_t s = base_status();
        s.state = T[i].s;
        char out[KVM_API_RESPONSE_MAX], want[40];
        snprintf(want, sizeof(want), "{\"state\":\"%s\",", T[i].name);
        TEST_ASSERT_EQUAL_INT(0, kvm_api_format_status(&s, out, sizeof(out), NULL));
        TEST_ASSERT_EQUAL_INT(0, strncmp(out, want, strlen(want)));
    }
}

void test_video_sync_names(void) {
    static const struct { kvm_video_sync_state_t s; const char *name; } T[] = {
        {KVM_VIDEO_SYNC_UNKNOWN, "unknown"}, {KVM_VIDEO_SYNC_NO_SIGNAL, "no_signal"}, {KVM_VIDEO_SYNC_LOCKED, "locked"},
    };
    for (size_t i = 0; i < sizeof(T) / sizeof(T[0]); i++) {
        kvm_ctrl_status_t s = base_status();
        s.video_sync = T[i].s;
        char out[KVM_API_RESPONSE_MAX], want[40];
        snprintf(want, sizeof(want), "\"video_sync\":\"%s\",", T[i].name);
        TEST_ASSERT_EQUAL_INT(0, kvm_api_format_status(&s, out, sizeof(out), NULL));
        TEST_ASSERT_NOT_NULL(strstr(out, want));
    }
}

void test_flags_are_independent(void) {
    for (int mask = 0; mask < 8; mask++) {
        kvm_ctrl_status_t s = base_status();
        s.atx_locked = (mask & 1) != 0;
        s.atx_anomaly = (mask & 2) != 0;
        s.fault = (mask & 4) != 0;
        char want[96];
        snprintf(want, sizeof(want), "\"atx_locked\":%s,\"atx_anomaly\":%s,\"fault\":%s,\"cooldown_ms\":0,",
                 s.atx_locked ? "true" : "false", s.atx_anomaly ? "true" : "false", s.fault ? "true" : "false");
        char out[KVM_API_RESPONSE_MAX];
        TEST_ASSERT_EQUAL_INT(0, kvm_api_format_status(&s, out, sizeof(out), NULL));
        TEST_ASSERT_NOT_NULL_MESSAGE(strstr(out, want), out);
    }
}

void test_cooldown_ms_values(void) {
    static const uint32_t V[] = {0, 1, 9, 10, 99, 100, 1999, 2000, 65535, 65536, 1000000000u, 4294967295u};
    for (size_t i = 0; i < sizeof(V) / sizeof(V[0]); i++) {
        kvm_ctrl_status_t s = base_status();
        s.cooldown_ms = V[i];
        char want[48], out[KVM_API_RESPONSE_MAX];
        snprintf(want, sizeof(want), "\"cooldown_ms\":%lu,", (unsigned long)V[i]);
        TEST_ASSERT_EQUAL_INT(0, kvm_api_format_status(&s, out, sizeof(out), NULL));
        TEST_ASSERT_NOT_NULL_MESSAGE(strstr(out, want), out);
    }
}

void test_last_op_all_kinds_and_results(void) {
    static const char *const K[] = {"select", "power_press", "power_reset", "power_force"};
    static const char *const R[] = {"pending", "completed", "aborted"};
    for (int k = 0; k < 4; k++) {
        for (int r = 0; r < 3; r++) {
            kvm_ctrl_status_t s = base_status();
            s.has_last_op = true;
            s.last_op.op_id = 4294967295u;
            s.last_op.kind = (kvm_ctrl_op_kind_t)k;
            s.last_op.channel = (uint8_t)(k + 1);
            s.last_op.result = (kvm_ctrl_op_result_t)r;
            char want[160], out[KVM_API_RESPONSE_MAX];
            snprintf(want, sizeof(want), "\"last_op\":{\"op_id\":4294967295,\"kind\":\"%s\",\"channel\":%d,\"result\":\"%s\"}}",
                     K[k], k + 1, R[r]);
            TEST_ASSERT_EQUAL_INT(0, kvm_api_format_status(&s, out, sizeof(out), NULL));
            TEST_ASSERT_NOT_NULL_MESSAGE(strstr(out, want), out);
        }
    }
}

void test_last_op_hidden_when_has_last_op_false(void) {
    kvm_ctrl_status_t s = base_status();
    s.has_last_op = false;
    s.last_op.op_id = 99;
    s.last_op.kind = KVM_CTRL_OP_SELECT;
    char out[KVM_API_RESPONSE_MAX];
    TEST_ASSERT_EQUAL_INT(0, kvm_api_format_status(&s, out, sizeof(out), NULL));
    TEST_ASSERT_NOT_NULL(strstr(out, ",\"last_op\":null}"));
}

/* 最長の JSON: すべてのフィールドが最大。バッファ KVM_API_RESPONSE_MAX に収まること、
 * 1 バイトでも足りなければ失敗して、出力が空文字列になること */
void test_buffer_boundaries(void) {
    kvm_ctrl_status_t s = base_status();
    s.state = KVM_CTRL_STATE_SWITCHING;
    s.video_sync = KVM_VIDEO_SYNC_NO_SIGNAL;
    s.atx_locked = true;
    s.atx_anomaly = true;
    s.fault = true;
    s.cooldown_ms = 4294967295u;
    s.has_last_op = true;
    s.last_op.op_id = 4294967295u;
    s.last_op.kind = KVM_CTRL_OP_POWER_RESET;
    s.last_op.channel = 4;
    s.last_op.result = KVM_CTRL_OP_COMPLETED;

    char full[KVM_API_RESPONSE_MAX];
    size_t need = 0;
    TEST_ASSERT_EQUAL_INT(0, kvm_api_format_status(&s, full, sizeof(full), &need));
    TEST_ASSERT_EQUAL_UINT((unsigned)strlen(full), (unsigned)need);
    TEST_ASSERT_TRUE(need + 1 < KVM_API_RESPONSE_MAX); /* 余裕がある */

    /* cap == need + 1 は成功、need 以下は全部失敗 */
    char *buf = malloc(need + 2);
    TEST_ASSERT_NOT_NULL(buf);
    for (size_t cap = 0; cap <= need + 1; cap++) {
        memset(buf, 'Z', need + 2);
        size_t len = 777;
        int r = kvm_api_format_status(&s, buf, cap, &len);
        if (cap >= need + 1) {
            TEST_ASSERT_EQUAL_INT(0, r);
            TEST_ASSERT_EQUAL_STRING(full, buf);
            TEST_ASSERT_EQUAL_UINT((unsigned)need, (unsigned)len);
        } else {
            TEST_ASSERT_EQUAL_INT(-1, r);
            TEST_ASSERT_EQUAL_UINT(0, (unsigned)len);
            if (cap > 0) TEST_ASSERT_EQUAL_CHAR('\0', buf[0]);
            /* cap より先は触っていない */
            for (size_t i = cap; i < need + 2; i++) TEST_ASSERT_EQUAL_CHAR('Z', buf[i]);
        }
    }
    free(buf);
}

void test_buffer_boundaries_for_short_json(void) {
    /* last_op: null で短い版でも、境界がぴったり */
    kvm_ctrl_status_t s = base_status();
    char full[KVM_API_RESPONSE_MAX];
    size_t need = 0;
    TEST_ASSERT_EQUAL_INT(0, kvm_api_format_status(&s, full, sizeof(full), &need));
    char *buf = malloc(need + 1);
    TEST_ASSERT_NOT_NULL(buf);
    TEST_ASSERT_EQUAL_INT(0, kvm_api_format_status(&s, buf, need + 1, NULL));
    TEST_ASSERT_EQUAL_INT(-1, kvm_api_format_status(&s, buf, need, NULL));
    free(buf);
}

void test_invalid_enum_values_fail(void) {
    char out[KVM_API_RESPONSE_MAX];
    kvm_ctrl_status_t s = base_status();
    s.state = (kvm_ctrl_state_t)99;
    memset(out, 'Z', sizeof(out));
    TEST_ASSERT_EQUAL_INT(-1, kvm_api_format_status(&s, out, sizeof(out), NULL));
    TEST_ASSERT_EQUAL_CHAR('\0', out[0]);

    s = base_status();
    s.video_sync = (kvm_video_sync_state_t)99;
    TEST_ASSERT_EQUAL_INT(-1, kvm_api_format_status(&s, out, sizeof(out), NULL));

    s = base_status();
    s.has_last_op = true;
    s.last_op.op_id = 1;
    s.last_op.kind = (kvm_ctrl_op_kind_t)99;
    s.last_op.result = KVM_CTRL_OP_PENDING;
    TEST_ASSERT_EQUAL_INT(-1, kvm_api_format_status(&s, out, sizeof(out), NULL));

    s.last_op.kind = KVM_CTRL_OP_SELECT;
    s.last_op.result = (kvm_ctrl_op_result_t)99;
    TEST_ASSERT_EQUAL_INT(-1, kvm_api_format_status(&s, out, sizeof(out), NULL));

    /* has_last_op が false なら、last_op の中身が壊れていても使わない */
    s.has_last_op = false;
    TEST_ASSERT_EQUAL_INT(0, kvm_api_format_status(&s, out, sizeof(out), NULL));
}

void test_null_arguments(void) {
    char out[64];
    kvm_ctrl_status_t s = base_status();
    TEST_ASSERT_EQUAL_INT(-1, kvm_api_format_status(NULL, out, sizeof(out), NULL));
    TEST_ASSERT_EQUAL_INT(-1, kvm_api_format_status(&s, NULL, 100, NULL));
    TEST_ASSERT_EQUAL_INT(-1, kvm_api_format_status(&s, out, 0, NULL));
    size_t len = 55;
    TEST_ASSERT_EQUAL_INT(-1, kvm_api_format_status(NULL, out, sizeof(out), &len));
    TEST_ASSERT_EQUAL_UINT(0, (unsigned)len);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_doc_example_exact);
    RUN_TEST(test_unknown_state_has_null_channel_and_no_last_op);
    RUN_TEST(test_unknown_state_hides_a_stale_channel);
    RUN_TEST(test_active_channel_out_of_range_is_null);
    RUN_TEST(test_each_active_channel);
    RUN_TEST(test_state_names);
    RUN_TEST(test_video_sync_names);
    RUN_TEST(test_flags_are_independent);
    RUN_TEST(test_cooldown_ms_values);
    RUN_TEST(test_last_op_all_kinds_and_results);
    RUN_TEST(test_last_op_hidden_when_has_last_op_false);
    RUN_TEST(test_buffer_boundaries);
    RUN_TEST(test_buffer_boundaries_for_short_json);
    RUN_TEST(test_invalid_enum_values_fail);
    RUN_TEST(test_null_arguments);
    return UNITY_END();
}
