#include "unity.h"
#include "http_guard.h"
#include <stdio.h>

/* http_guard_decide() の KVM / ATX パス（docs/WEB_API.md 7 章）の表のテスト。
 *   READY のみ。ホスト -> (NEEDS_SETUP は 403 SETUP_REQUIRED) -> セッション(401) -> メソッド(405) -> Origin(403) -> JSON(415)。
 *   status は GET のみ（Origin 不要: 同一オリジンの GET に Origin は付かない）。select と power は POST のみ。 */

void setUp(void) {}
void tearDown(void) {}

static bool is_kvm_path(http_guard_path_e p) {
    return p == HTTP_GUARD_PATH_KVM_STATUS || p == HTTP_GUARD_PATH_KVM_SELECT || p == HTTP_GUARD_PATH_POWER;
}

/* 仕様から独立に書いた期待値 */
static http_guard_action_e expected(auth_state_e state, http_guard_method_e m, http_guard_path_e p, bool session, bool origin,
                                    bool json, bool host) {
    if (!host) return HTTP_GUARD_ACTION_403_HOST;
    if (state != AUTH_STATE_READY) return HTTP_GUARD_ACTION_403_SETUP_REQUIRED;
    if (!session) return HTTP_GUARD_ACTION_401;
    if (p == HTTP_GUARD_PATH_KVM_STATUS) {
        return m == HTTP_GUARD_METHOD_GET ? HTTP_GUARD_ACTION_ALLOW : HTTP_GUARD_ACTION_405;
    }
    /* select / power */
    if (m != HTTP_GUARD_METHOD_POST) return HTTP_GUARD_ACTION_405;
    if (!origin) return HTTP_GUARD_ACTION_403_ORIGIN;
    if (!json) return HTTP_GUARD_ACTION_415;
    return HTTP_GUARD_ACTION_ALLOW;
}

void test_exhaustive_kvm_paths(void) {
    const auth_state_e states[] = {AUTH_STATE_NEEDS_SETUP, AUTH_STATE_READY};
    const http_guard_method_e methods[] = {HTTP_GUARD_METHOD_GET, HTTP_GUARD_METHOD_POST, HTTP_GUARD_METHOD_OTHER};
    const http_guard_path_e paths[] = {HTTP_GUARD_PATH_KVM_STATUS, HTTP_GUARD_PATH_KVM_SELECT, HTTP_GUARD_PATH_POWER};
    int allowed = 0, total = 0;
    for (int is = 0; is < 2; is++)
        for (int im = 0; im < 3; im++)
            for (int ip = 0; ip < 3; ip++)
                for (int bits = 0; bits < 16; bits++) {
                    bool session = (bits & 1) != 0, origin = (bits & 2) != 0, json = (bits & 4) != 0, host = (bits & 8) != 0;
                    http_guard_action_e got = http_guard_decide(states[is], methods[im], paths[ip], session, origin, json, host);
                    http_guard_action_e want = expected(states[is], methods[im], paths[ip], session, origin, json, host);
                    total++;
                    if (got == HTTP_GUARD_ACTION_ALLOW) allowed++;
                    if (got != want) {
                        char msg[160];
                        snprintf(msg, sizeof(msg), "state=%d method=%d path=%d session=%d origin=%d json=%d host=%d got=%d want=%d",
                                 states[is], methods[im], paths[ip], session, origin, json, host, got, want);
                        TEST_FAIL_MESSAGE(msg);
                    }
                }
    TEST_ASSERT_EQUAL_INT(2 * 3 * 3 * 16, total);
    /* 許可されるのは: READY かつ host かつ session で、status は GET(origin/json は任意で 4 通り)、select/power は POST + origin + json */
    TEST_ASSERT_EQUAL_INT(4 + 1 + 1, allowed);
}

/* 仕様の言葉で直接確かめる(上の表が expected() の書き間違いと一致しているだけ、を避ける) */
void test_allow_requires_everything(void) {
    const auth_state_e R = AUTH_STATE_READY;
    TEST_ASSERT_EQUAL(HTTP_GUARD_ACTION_ALLOW, http_guard_decide(R, HTTP_GUARD_METHOD_GET, HTTP_GUARD_PATH_KVM_STATUS, true, false, false, true));
    TEST_ASSERT_EQUAL(HTTP_GUARD_ACTION_ALLOW, http_guard_decide(R, HTTP_GUARD_METHOD_POST, HTTP_GUARD_PATH_KVM_SELECT, true, true, true, true));
    TEST_ASSERT_EQUAL(HTTP_GUARD_ACTION_ALLOW, http_guard_decide(R, HTTP_GUARD_METHOD_POST, HTTP_GUARD_PATH_POWER, true, true, true, true));
}

void test_unauthenticated_is_401_on_every_kvm_path_and_method(void) {
    const http_guard_path_e paths[] = {HTTP_GUARD_PATH_KVM_STATUS, HTTP_GUARD_PATH_KVM_SELECT, HTTP_GUARD_PATH_POWER};
    const http_guard_method_e methods[] = {HTTP_GUARD_METHOD_GET, HTTP_GUARD_METHOD_POST, HTTP_GUARD_METHOD_OTHER};
    for (int ip = 0; ip < 3; ip++)
        for (int im = 0; im < 3; im++)
            for (int bits = 0; bits < 4; bits++) { /* origin, json は何であっても */
                TEST_ASSERT_EQUAL(HTTP_GUARD_ACTION_401,
                                  http_guard_decide(AUTH_STATE_READY, methods[im], paths[ip], false, (bits & 1) != 0, (bits & 2) != 0, true));
            }
}

void test_needs_setup_blocks_every_kvm_path(void) {
    const http_guard_path_e paths[] = {HTTP_GUARD_PATH_KVM_STATUS, HTTP_GUARD_PATH_KVM_SELECT, HTTP_GUARD_PATH_POWER};
    for (int ip = 0; ip < 3; ip++) {
        /* セッションがあって、Origin も JSON も正しくても、セットアップ前は 403 SETUP_REQUIRED */
        TEST_ASSERT_EQUAL(HTTP_GUARD_ACTION_403_SETUP_REQUIRED,
                          http_guard_decide(AUTH_STATE_NEEDS_SETUP, HTTP_GUARD_METHOD_POST, paths[ip], true, true, true, true));
        TEST_ASSERT_EQUAL(HTTP_GUARD_ACTION_403_SETUP_REQUIRED,
                          http_guard_decide(AUTH_STATE_NEEDS_SETUP, HTTP_GUARD_METHOD_GET, paths[ip], true, true, true, true));
        TEST_ASSERT_EQUAL(HTTP_GUARD_ACTION_403_SETUP_REQUIRED,
                          http_guard_decide(AUTH_STATE_NEEDS_SETUP, HTTP_GUARD_METHOD_OTHER, paths[ip], true, true, true, true));
    }
}

void test_bad_host_beats_everything(void) {
    const http_guard_path_e paths[] = {HTTP_GUARD_PATH_KVM_STATUS, HTTP_GUARD_PATH_KVM_SELECT, HTTP_GUARD_PATH_POWER};
    for (int ip = 0; ip < 3; ip++)
        for (int is = 0; is < 2; is++)
            TEST_ASSERT_EQUAL(HTTP_GUARD_ACTION_403_HOST,
                              http_guard_decide(is ? AUTH_STATE_READY : AUTH_STATE_NEEDS_SETUP, HTTP_GUARD_METHOD_POST, paths[ip],
                                                false, false, false, false));
}

void test_check_order_session_method_origin_json(void) {
    const auth_state_e R = AUTH_STATE_READY;
    /* セッション無し + 誤ったメソッド: 401（セッションが先） */
    TEST_ASSERT_EQUAL(HTTP_GUARD_ACTION_401, http_guard_decide(R, HTTP_GUARD_METHOD_GET, HTTP_GUARD_PATH_KVM_SELECT, false, true, true, true));
    /* 誤ったメソッド + Origin 無し + JSON でない: 405（メソッドが Origin より先） */
    TEST_ASSERT_EQUAL(HTTP_GUARD_ACTION_405, http_guard_decide(R, HTTP_GUARD_METHOD_GET, HTTP_GUARD_PATH_KVM_SELECT, true, false, false, true));
    TEST_ASSERT_EQUAL(HTTP_GUARD_ACTION_405, http_guard_decide(R, HTTP_GUARD_METHOD_GET, HTTP_GUARD_PATH_POWER, true, false, false, true));
    TEST_ASSERT_EQUAL(HTTP_GUARD_ACTION_405, http_guard_decide(R, HTTP_GUARD_METHOD_OTHER, HTTP_GUARD_PATH_POWER, true, false, false, true));
    TEST_ASSERT_EQUAL(HTTP_GUARD_ACTION_405, http_guard_decide(R, HTTP_GUARD_METHOD_POST, HTTP_GUARD_PATH_KVM_STATUS, true, true, true, true));
    TEST_ASSERT_EQUAL(HTTP_GUARD_ACTION_405, http_guard_decide(R, HTTP_GUARD_METHOD_OTHER, HTTP_GUARD_PATH_KVM_STATUS, true, true, true, true));
    /* Origin 無し + JSON でない: 403 Origin（Origin が JSON より先） */
    TEST_ASSERT_EQUAL(HTTP_GUARD_ACTION_403_ORIGIN, http_guard_decide(R, HTTP_GUARD_METHOD_POST, HTTP_GUARD_PATH_KVM_SELECT, true, false, false, true));
    TEST_ASSERT_EQUAL(HTTP_GUARD_ACTION_403_ORIGIN, http_guard_decide(R, HTTP_GUARD_METHOD_POST, HTTP_GUARD_PATH_POWER, true, false, false, true));
    TEST_ASSERT_EQUAL(HTTP_GUARD_ACTION_403_ORIGIN, http_guard_decide(R, HTTP_GUARD_METHOD_POST, HTTP_GUARD_PATH_POWER, true, false, true, true));
    /* Origin はあるが JSON でない: 415 */
    TEST_ASSERT_EQUAL(HTTP_GUARD_ACTION_415, http_guard_decide(R, HTTP_GUARD_METHOD_POST, HTTP_GUARD_PATH_KVM_SELECT, true, true, false, true));
    TEST_ASSERT_EQUAL(HTTP_GUARD_ACTION_415, http_guard_decide(R, HTTP_GUARD_METHOD_POST, HTTP_GUARD_PATH_POWER, true, true, false, true));
}

void test_status_is_get_only_and_needs_no_origin_or_json(void) {
    const auth_state_e R = AUTH_STATE_READY;
    for (int bits = 0; bits < 4; bits++) {
        TEST_ASSERT_EQUAL(HTTP_GUARD_ACTION_ALLOW,
                          http_guard_decide(R, HTTP_GUARD_METHOD_GET, HTTP_GUARD_PATH_KVM_STATUS, true, (bits & 1) != 0, (bits & 2) != 0, true));
    }
}

void test_select_and_power_do_not_allow_get(void) {
    const auth_state_e R = AUTH_STATE_READY;
    TEST_ASSERT_EQUAL(HTTP_GUARD_ACTION_405, http_guard_decide(R, HTTP_GUARD_METHOD_GET, HTTP_GUARD_PATH_KVM_SELECT, true, true, true, true));
    TEST_ASSERT_EQUAL(HTTP_GUARD_ACTION_405, http_guard_decide(R, HTTP_GUARD_METHOD_GET, HTTP_GUARD_PATH_POWER, true, true, true, true));
}

/* 新しい列挙値を足しても、既存のパスの意味が変わらない(OTHER は末尾のまま、値は重ならない) */
void test_enum_values_are_distinct_and_other_is_last(void) {
    TEST_ASSERT_TRUE(HTTP_GUARD_PATH_KVM_STATUS != HTTP_GUARD_PATH_KVM_SELECT);
    TEST_ASSERT_TRUE(HTTP_GUARD_PATH_KVM_STATUS != HTTP_GUARD_PATH_POWER);
    TEST_ASSERT_TRUE(HTTP_GUARD_PATH_KVM_SELECT != HTTP_GUARD_PATH_POWER);
    TEST_ASSERT_TRUE(HTTP_GUARD_PATH_OTHER > HTTP_GUARD_PATH_KVM_STATUS);
    TEST_ASSERT_TRUE(HTTP_GUARD_PATH_OTHER > HTTP_GUARD_PATH_KVM_SELECT);
    TEST_ASSERT_TRUE(HTTP_GUARD_PATH_OTHER > HTTP_GUARD_PATH_POWER);
    TEST_ASSERT_TRUE(HTTP_GUARD_PATH_OTHER > HTTP_GUARD_PATH_STATIC);
    TEST_ASSERT_FALSE(is_kvm_path(HTTP_GUARD_PATH_OTHER));
    TEST_ASSERT_FALSE(is_kvm_path(HTTP_GUARD_PATH_STATIC));
    TEST_ASSERT_FALSE(is_kvm_path(HTTP_GUARD_PATH_WS_INPUT));
}

/* 既存のパスの挙動が変わっていない(代表) */
void test_existing_paths_unchanged(void) {
    const auth_state_e R = AUTH_STATE_READY;
    TEST_ASSERT_EQUAL(HTTP_GUARD_ACTION_401, http_guard_decide(R, HTTP_GUARD_METHOD_GET, HTTP_GUARD_PATH_OTHER, false, true, true, true));
    TEST_ASSERT_EQUAL(HTTP_GUARD_ACTION_ALLOW, http_guard_decide(R, HTTP_GUARD_METHOD_GET, HTTP_GUARD_PATH_OTHER, true, false, false, true));
    TEST_ASSERT_EQUAL(HTTP_GUARD_ACTION_403_ORIGIN, http_guard_decide(R, HTTP_GUARD_METHOD_POST, HTTP_GUARD_PATH_OTHER, true, false, true, true));
    TEST_ASSERT_EQUAL(HTTP_GUARD_ACTION_ALLOW, http_guard_decide(R, HTTP_GUARD_METHOD_GET, HTTP_GUARD_PATH_WS_INPUT, true, true, false, true));
    TEST_ASSERT_EQUAL(HTTP_GUARD_ACTION_ALLOW, http_guard_decide(R, HTTP_GUARD_METHOD_POST, HTTP_GUARD_PATH_LOGIN, false, true, true, true));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_exhaustive_kvm_paths);
    RUN_TEST(test_allow_requires_everything);
    RUN_TEST(test_unauthenticated_is_401_on_every_kvm_path_and_method);
    RUN_TEST(test_needs_setup_blocks_every_kvm_path);
    RUN_TEST(test_bad_host_beats_everything);
    RUN_TEST(test_check_order_session_method_origin_json);
    RUN_TEST(test_status_is_get_only_and_needs_no_origin_or_json);
    RUN_TEST(test_select_and_power_do_not_allow_get);
    RUN_TEST(test_enum_values_are_distinct_and_other_is_last);
    RUN_TEST(test_existing_paths_unchanged);
    return UNITY_END();
}
