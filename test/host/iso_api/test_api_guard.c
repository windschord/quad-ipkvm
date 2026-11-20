/*
 * 段 1(認証)と段 2(Origin)は、iso_api の前(web_server)で、http_guard_decide が行う。ISO の API は、次のように呼ぶ(web_iso.c):
 *   method: GET は GET、POST / PUT / DELETE は POST(変更する操作。Origin が必須)。path: HTTP_GUARD_PATH_OTHER。is_json: 真(415 は iso_api が返す)。
 * この前提(未設定 403、Host 403、認証なし 401、Origin 403 の順。GET は Origin 不要)を、テストで固定する。
 */
#include "unity.h"
#include "http_guard.h"

void setUp(void) {}
void tearDown(void) {}

static http_guard_action_e decide(auth_state_e st, http_guard_method_e m, bool session, bool origin_ok, bool host_ok) {
    return http_guard_decide(st, m, HTTP_GUARD_PATH_OTHER, session, origin_ok, true, host_ok);
}

void test_step1_host_then_setup_then_authentication(void) {
    /* Host が違えば、まず 403(Host) */
    TEST_ASSERT_EQUAL_INT(HTTP_GUARD_ACTION_403_HOST, decide(AUTH_STATE_READY, HTTP_GUARD_METHOD_GET, true, true, false));
    TEST_ASSERT_EQUAL_INT(HTTP_GUARD_ACTION_403_HOST, decide(AUTH_STATE_READY, HTTP_GUARD_METHOD_POST, false, false, false));
    /* 初期設定の前は 403(setup_required) */
    TEST_ASSERT_EQUAL_INT(HTTP_GUARD_ACTION_403_SETUP_REQUIRED, decide(AUTH_STATE_NEEDS_SETUP, HTTP_GUARD_METHOD_GET, true, true, true));
    TEST_ASSERT_EQUAL_INT(HTTP_GUARD_ACTION_403_SETUP_REQUIRED, decide(AUTH_STATE_NEEDS_SETUP, HTTP_GUARD_METHOD_POST, true, true, true));
    /* 認証なしは 401(Origin より前) */
    TEST_ASSERT_EQUAL_INT(HTTP_GUARD_ACTION_401, decide(AUTH_STATE_READY, HTTP_GUARD_METHOD_GET, false, true, true));
    TEST_ASSERT_EQUAL_INT(HTTP_GUARD_ACTION_401, decide(AUTH_STATE_READY, HTTP_GUARD_METHOD_POST, false, false, true));
    TEST_ASSERT_EQUAL_INT(HTTP_GUARD_ACTION_401, decide(AUTH_STATE_READY, HTTP_GUARD_METHOD_POST, false, true, true));
}

void test_step2_origin_is_required_for_changing_requests_only(void) {
    /* GET は Origin なしで通る(既存の GET と同じ) */
    TEST_ASSERT_EQUAL_INT(HTTP_GUARD_ACTION_ALLOW, decide(AUTH_STATE_READY, HTTP_GUARD_METHOD_GET, true, false, true));
    TEST_ASSERT_EQUAL_INT(HTTP_GUARD_ACTION_ALLOW, decide(AUTH_STATE_READY, HTTP_GUARD_METHOD_GET, true, true, true));
    /* POST / PUT / DELETE(POST として判定)は、Origin が必須 */
    TEST_ASSERT_EQUAL_INT(HTTP_GUARD_ACTION_403_ORIGIN, decide(AUTH_STATE_READY, HTTP_GUARD_METHOD_POST, true, false, true));
    TEST_ASSERT_EQUAL_INT(HTTP_GUARD_ACTION_ALLOW, decide(AUTH_STATE_READY, HTTP_GUARD_METHOD_POST, true, true, true));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_step1_host_then_setup_then_authentication);
    RUN_TEST(test_step2_origin_is_required_for_changing_requests_only);
    return UNITY_END();
}
