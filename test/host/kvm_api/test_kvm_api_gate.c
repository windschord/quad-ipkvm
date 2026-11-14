#include "ka_common.h"
#include "http_guard.h"

/*
 * 「未認証・Origin なし・JSON でないリクエストが kvm_api に届かない」ことの固定。
 *
 * web_server.c のハンドラは (1) http_guard_decide() が ALLOW のときだけ (2) kvm_api_handle() を呼ぶ。
 * ここでは同じ手順を dispatch() に写し、decide への全入力(状態 x メソッド x パス x セッション x Origin x JSON x Host)で、
 * モック backend が ALLOW のときだけ触られることを確かめる。ALLOW の条件は、仕様の言葉(認証・Origin・JSON)で書き直した式。
 * （web_server.c 自体が、この手順どおりに書かれているかは、ホストでは確かめられない。IDF ビルドと実機で確認する。）
 */

void setUp(void) { be_reset(); }
void tearDown(void) {}

static http_guard_action_e g_last_action;

/* web_server.c の kvm ハンドラの手順: decide -> ALLOW のときだけ kvm_api_handle */
static void dispatch(auth_state_e state, http_guard_method_e method, http_guard_path_e path, bool session, bool origin,
                     bool json, bool host, const char *body) {
    kvm_api_backend_t t = be_table();
    g_last_action = http_guard_decide(state, method, path, session, origin, json, host);
    if (g_last_action != HTTP_GUARD_ACTION_ALLOW) return;
    kvm_api_endpoint_t ep = (path == HTTP_GUARD_PATH_KVM_STATUS)   ? KVM_API_EP_STATUS
                            : (path == HTTP_GUARD_PATH_KVM_SELECT) ? KVM_API_EP_SELECT
                                                                   : KVM_API_EP_POWER_FORCE;
    kvm_api_handle(&t, ep, body, body ? strlen(body) : 0, &RSP);
}

static int touched(void) {
    return B.ready_calls + B.lock_calls + B.unlock_calls + B.status_calls + B.select_calls + B.power_calls;
}

void test_backend_is_touched_only_when_every_condition_holds(void) {
    const auth_state_e states[] = {AUTH_STATE_NEEDS_SETUP, AUTH_STATE_READY};
    const http_guard_method_e methods[] = {HTTP_GUARD_METHOD_GET, HTTP_GUARD_METHOD_POST, HTTP_GUARD_METHOD_OTHER};
    const http_guard_path_e paths[] = {HTTP_GUARD_PATH_KVM_STATUS, HTTP_GUARD_PATH_KVM_SELECT, HTTP_GUARD_PATH_POWER};
    int reached = 0;
    for (int is = 0; is < 2; is++)
        for (int im = 0; im < 3; im++)
            for (int ip = 0; ip < 3; ip++)
                for (int bits = 0; bits < 16; bits++) {
                    bool session = (bits & 1) != 0, origin = (bits & 2) != 0, json = (bits & 4) != 0, host = (bits & 8) != 0;
                    be_reset();
                    dispatch(states[is], methods[im], paths[ip], session, origin, json, host, "{\"channel\":2}");

                    /* 仕様の言葉: READY で、Host が正しく、有効なセッションがあり、
                     * status は GET、select / power は POST + Origin + JSON のときだけ */
                    bool want = (states[is] == AUTH_STATE_READY) && host && session;
                    if (paths[ip] == HTTP_GUARD_PATH_KVM_STATUS) {
                        want = want && methods[im] == HTTP_GUARD_METHOD_GET;
                    } else {
                        want = want && methods[im] == HTTP_GUARD_METHOD_POST && origin && json;
                    }
                    char msg[128];
                    snprintf(msg, sizeof(msg), "state=%d method=%d path=%d session=%d origin=%d json=%d host=%d", states[is],
                             methods[im], paths[ip], session, origin, json, host);
                    TEST_ASSERT_EQUAL_MESSAGE(want, touched() > 0, msg);
                    TEST_ASSERT_EQUAL_MESSAGE(want, g_last_action == HTTP_GUARD_ACTION_ALLOW, msg);
                    if (want) reached++;
                    if (!want) TEST_ASSERT_EQUAL_INT_MESSAGE(0, touched(), msg); /* ready() すら呼ばれない */
                }
    TEST_ASSERT_EQUAL_INT(6, reached);
}

void test_unauthenticated_post_never_reaches_the_hardware_calls(void) {
    /* 攻撃者の POST: セッション無し。select も power も呼ばれない */
    dispatch(AUTH_STATE_READY, HTTP_GUARD_METHOD_POST, HTTP_GUARD_PATH_POWER, false, true, true, true, "{\"channel\":2}");
    TEST_ASSERT_EQUAL(HTTP_GUARD_ACTION_401, g_last_action);
    dispatch(AUTH_STATE_READY, HTTP_GUARD_METHOD_POST, HTTP_GUARD_PATH_KVM_SELECT, false, true, true, true, "{\"channel\":2}");
    TEST_ASSERT_EQUAL(HTTP_GUARD_ACTION_401, g_last_action);
    /* CSRF: セッションはあるが Origin が無い / 一致しない */
    dispatch(AUTH_STATE_READY, HTTP_GUARD_METHOD_POST, HTTP_GUARD_PATH_POWER, true, false, true, true, "{\"channel\":2}");
    TEST_ASSERT_EQUAL(HTTP_GUARD_ACTION_403_ORIGIN, g_last_action);
    /* Content-Type が JSON でない(form 送信など) */
    dispatch(AUTH_STATE_READY, HTTP_GUARD_METHOD_POST, HTTP_GUARD_PATH_POWER, true, true, false, true, "{\"channel\":2}");
    TEST_ASSERT_EQUAL(HTTP_GUARD_ACTION_415, g_last_action);
    TEST_ASSERT_EQUAL_INT(0, touched());

    /* 正しいリクエストは届く */
    dispatch(AUTH_STATE_READY, HTTP_GUARD_METHOD_POST, HTTP_GUARD_PATH_POWER, true, true, true, true, "{\"channel\":2}");
    TEST_ASSERT_EQUAL_INT(1, B.power_calls);
    TEST_ASSERT_EQUAL_INT(202, RSP.status);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_backend_is_touched_only_when_every_condition_holds);
    RUN_TEST(test_unauthenticated_post_never_reaches_the_hardware_calls);
    return UNITY_END();
}
