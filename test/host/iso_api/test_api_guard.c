/*
 * 段 1(認証)と段 2(Origin)は、iso_api の前(web_server)で、http_guard_decide が行う。ISO の API は、次のように呼ぶ(web_iso.c):
 *   method: GET は GET、POST / PUT / DELETE は POST(変更する操作。Origin が必須)。path: HTTP_GUARD_PATH_OTHER。is_json: 真(415 は iso_api が返す)。
 * この前提(未設定 403、Host 403、認証なし 401、Origin 403 の順。GET は Origin 不要)を、テストで固定する。
 */
#include "unity.h"
#include "http_guard.h"
#include <stdio.h>

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

/* Host と Origin は、別々の入力(web_server_iso_guard は、別々に検査して渡す)。全部の組み合わせ(GET / POST × 認証 × Origin × Host)の期待値を、手書きで固定する。
 * 優先: Host(403 forbidden_host) → 初期設定(403 setup_required) → 認証(401) → Origin(403 forbidden_origin。POST だけ) */
void test_every_combination_of_host_session_and_origin(void) {
    static const struct {
        http_guard_method_e m;
        bool session, origin_ok, host_ok;
        http_guard_action_e want;
    } T[] = {
        /* GET: Origin は見ない */
        {HTTP_GUARD_METHOD_GET, true, true, true, HTTP_GUARD_ACTION_ALLOW},
        {HTTP_GUARD_METHOD_GET, true, false, true, HTTP_GUARD_ACTION_ALLOW},
        {HTTP_GUARD_METHOD_GET, false, true, true, HTTP_GUARD_ACTION_401},
        {HTTP_GUARD_METHOD_GET, false, false, true, HTTP_GUARD_ACTION_401},
        {HTTP_GUARD_METHOD_GET, true, true, false, HTTP_GUARD_ACTION_403_HOST},
        {HTTP_GUARD_METHOD_GET, true, false, false, HTTP_GUARD_ACTION_403_HOST},
        {HTTP_GUARD_METHOD_GET, false, true, false, HTTP_GUARD_ACTION_403_HOST},
        {HTTP_GUARD_METHOD_GET, false, false, false, HTTP_GUARD_ACTION_403_HOST},
        /* POST(PUT / DELETE も、POST として判定) */
        {HTTP_GUARD_METHOD_POST, true, true, true, HTTP_GUARD_ACTION_ALLOW},
        {HTTP_GUARD_METHOD_POST, true, false, true, HTTP_GUARD_ACTION_403_ORIGIN}, /* 有効なセッションで、Origin だけが不正: forbidden_origin */
        {HTTP_GUARD_METHOD_POST, false, true, true, HTTP_GUARD_ACTION_401},
        {HTTP_GUARD_METHOD_POST, false, false, true, HTTP_GUARD_ACTION_401},      /* 許可された Host の未認証 POST で Origin がない: 401(forbidden_host ではない) */
        {HTTP_GUARD_METHOD_POST, true, true, false, HTTP_GUARD_ACTION_403_HOST},
        {HTTP_GUARD_METHOD_POST, true, false, false, HTTP_GUARD_ACTION_403_HOST},
        {HTTP_GUARD_METHOD_POST, false, true, false, HTTP_GUARD_ACTION_403_HOST},
        {HTTP_GUARD_METHOD_POST, false, false, false, HTTP_GUARD_ACTION_403_HOST},
    };
    for (size_t i = 0; i < sizeof(T) / sizeof(T[0]); i++) {
        char msg[48];
        snprintf(msg, sizeof(msg), "row %zu", i);
        TEST_ASSERT_EQUAL_INT_MESSAGE(T[i].want, decide(AUTH_STATE_READY, T[i].m, T[i].session, T[i].origin_ok, T[i].host_ok), msg);
    }
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_step1_host_then_setup_then_authentication);
    RUN_TEST(test_step2_origin_is_required_for_changing_requests_only);
    RUN_TEST(test_every_combination_of_host_session_and_origin);
    return UNITY_END();
}
