#include "unity.h"
#include "http_guard.h"
#include <stdio.h>

void setUp(void) {}
void tearDown(void) {}

static http_guard_action_e expected_decide(auth_state_e state, http_guard_method_e method, http_guard_path_e path, bool has_valid_session, bool origin_ok, bool is_json, bool host_ok) {
    if (!host_ok) return HTTP_GUARD_ACTION_403_HOST;

    if (state == AUTH_STATE_NEEDS_SETUP) {
        if (path == HTTP_GUARD_PATH_STATUS) return (method == HTTP_GUARD_METHOD_GET) ? HTTP_GUARD_ACTION_ALLOW : HTTP_GUARD_ACTION_405;
        if (path == HTTP_GUARD_PATH_STATIC) return (method == HTTP_GUARD_METHOD_GET) ? HTTP_GUARD_ACTION_ALLOW : HTTP_GUARD_ACTION_405;
        if (path == HTTP_GUARD_PATH_SETUP) {
            if (method != HTTP_GUARD_METHOD_POST) return HTTP_GUARD_ACTION_405;
            if (!origin_ok) return HTTP_GUARD_ACTION_403_ORIGIN;
            if (!is_json) return HTTP_GUARD_ACTION_415;
            return HTTP_GUARD_ACTION_ALLOW;
        }
        return HTTP_GUARD_ACTION_403_SETUP_REQUIRED;
    } else { // AUTH_STATE_READY
        if (path == HTTP_GUARD_PATH_STATUS) return (method == HTTP_GUARD_METHOD_GET) ? HTTP_GUARD_ACTION_ALLOW : HTTP_GUARD_ACTION_405;
        if (path == HTTP_GUARD_PATH_STATIC) return (method == HTTP_GUARD_METHOD_GET) ? HTTP_GUARD_ACTION_ALLOW : HTTP_GUARD_ACTION_405;
        if (path == HTTP_GUARD_PATH_SETUP) return HTTP_GUARD_ACTION_409_ALREADY_SETUP;
        if (path == HTTP_GUARD_PATH_LOGIN) {
            if (method != HTTP_GUARD_METHOD_POST) return HTTP_GUARD_ACTION_405;
            if (!origin_ok) return HTTP_GUARD_ACTION_403_ORIGIN;
            if (!is_json) return HTTP_GUARD_ACTION_415;
            return HTTP_GUARD_ACTION_ALLOW;
        }

        if (!has_valid_session) return HTTP_GUARD_ACTION_401;

        if (path == HTTP_GUARD_PATH_LOGOUT) {
            if (method != HTTP_GUARD_METHOD_POST) return HTTP_GUARD_ACTION_405;
            if (!origin_ok) return HTTP_GUARD_ACTION_403_ORIGIN;
            return HTTP_GUARD_ACTION_ALLOW;
        }

        if (path == HTTP_GUARD_PATH_WS_VIDEO) {
            if (method != HTTP_GUARD_METHOD_GET) return HTTP_GUARD_ACTION_405;
            if (!origin_ok) return HTTP_GUARD_ACTION_403_ORIGIN;
            return HTTP_GUARD_ACTION_ALLOW;
        }

        if (path == HTTP_GUARD_PATH_OTHER) {
            if (method == HTTP_GUARD_METHOD_POST) {
                if (!origin_ok) return HTTP_GUARD_ACTION_403_ORIGIN;
                return HTTP_GUARD_ACTION_ALLOW;
            } else if (method == HTTP_GUARD_METHOD_GET) {
                return HTTP_GUARD_ACTION_ALLOW;
            } else {
                return HTTP_GUARD_ACTION_405;
            }
        }
    }
    return HTTP_GUARD_ACTION_ALLOW;
}

void test_exhaustive_matrix_and_invariants(void) {
    auth_state_e states[] = {AUTH_STATE_NEEDS_SETUP, AUTH_STATE_READY};
    http_guard_method_e methods[] = {HTTP_GUARD_METHOD_GET, HTTP_GUARD_METHOD_POST, HTTP_GUARD_METHOD_OTHER};
    http_guard_path_e paths[] = {HTTP_GUARD_PATH_STATUS, HTTP_GUARD_PATH_SETUP, HTTP_GUARD_PATH_LOGIN, HTTP_GUARD_PATH_LOGOUT, HTTP_GUARD_PATH_WS_VIDEO, HTTP_GUARD_PATH_STATIC, HTTP_GUARD_PATH_OTHER};
    bool bools[] = {false, true};

    for (int is = 0; is < 2; is++) {
        for (int im = 0; im < 3; im++) {
            for (int ip = 0; ip < 7; ip++) {
                for (int iv = 0; iv < 2; iv++) {
                    for (int io = 0; io < 2; io++) {
                        for (int ij = 0; ij < 2; ij++) {
                            for (int ih = 0; ih < 2; ih++) {
                                auth_state_e state = states[is];
                                http_guard_method_e method = methods[im];
                                http_guard_path_e path = paths[ip];
                                bool has_valid_session = bools[iv];
                                bool origin_ok = bools[io];
                                bool is_json = bools[ij];
                                bool host_ok = bools[ih];

                                http_guard_action_e actual = http_guard_decide(state, method, path, has_valid_session, origin_ok, is_json, host_ok);
                                http_guard_action_e expected = expected_decide(state, method, path, has_valid_session, origin_ok, is_json, host_ok);

                                if (actual != expected) {
                                    printf("Failed config: state=%d method=%d path=%d session=%d origin=%d json=%d host=%d\n", state, method, path, has_valid_session, origin_ok, is_json, host_ok);
                                }
                                TEST_ASSERT_EQUAL(expected, actual);

                                if (!host_ok) {
                                    TEST_ASSERT_NOT_EQUAL(HTTP_GUARD_ACTION_ALLOW, actual);
                                }

                                if (host_ok) {
                                    if (state == AUTH_STATE_NEEDS_SETUP && actual == HTTP_GUARD_ACTION_ALLOW) {
                                        TEST_ASSERT_TRUE(path == HTTP_GUARD_PATH_STATUS || path == HTTP_GUARD_PATH_SETUP || path == HTTP_GUARD_PATH_STATIC);
                                    }

                                    if (state == AUTH_STATE_READY && actual == HTTP_GUARD_ACTION_ALLOW) {
                                        if (path != HTTP_GUARD_PATH_STATUS && path != HTTP_GUARD_PATH_LOGIN && path != HTTP_GUARD_PATH_STATIC) {
                                            TEST_ASSERT_TRUE(has_valid_session);
                                        }
                                    }

                                    if (actual == HTTP_GUARD_ACTION_ALLOW) {
                                        if (method != HTTP_GUARD_METHOD_GET || path == HTTP_GUARD_PATH_WS_VIDEO) {
                                            TEST_ASSERT_TRUE(origin_ok);
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_exhaustive_matrix_and_invariants);
    return UNITY_END();
}
