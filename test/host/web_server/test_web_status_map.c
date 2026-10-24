#include "unity.h"
#include "web_status_map.h"
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

void test_login_success(void) {
    auth_logic_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    TEST_ASSERT_EQUAL(200, web_status_map_login(0, &ctx, 1234, 1000));
}

void test_login_rate_limited(void) {
    auth_logic_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.rate_limits[0].source_ip = 1234;
    ctx.rate_limits[0].lockout_until_ms = 2000;
    TEST_ASSERT_EQUAL(429, web_status_map_login(-1, &ctx, 1234, 1000));
}

void test_login_rate_limited_global(void) {
    auth_logic_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.global_lockout_until_ms = 2000;
    TEST_ASSERT_EQUAL(429, web_status_map_login(-1, &ctx, 1234, 1000));
}

void test_login_rate_limited_expired(void) {
    auth_logic_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.rate_limits[0].source_ip = 1234;
    ctx.rate_limits[0].lockout_until_ms = 500;
    TEST_ASSERT_EQUAL(401, web_status_map_login(-1, &ctx, 1234, 1000));
}

void test_login_rate_limited_different_ip(void) {
    auth_logic_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.rate_limits[0].source_ip = 5678;
    ctx.rate_limits[0].lockout_until_ms = 2000;
    TEST_ASSERT_EQUAL(401, web_status_map_login(-1, &ctx, 1234, 1000));
}

void test_login_unauthorized(void) {
    auth_logic_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    TEST_ASSERT_EQUAL(401, web_status_map_login(-1, &ctx, 1234, 1000));
}

void test_setup_success(void) {
    auth_logic_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.state = AUTH_STATE_NEEDS_SETUP;
    TEST_ASSERT_EQUAL(200, web_status_map_setup(0, &ctx));
}

void test_setup_forbidden(void) {
    auth_logic_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.state = AUTH_STATE_READY;
    TEST_ASSERT_EQUAL(403, web_status_map_setup(-1, &ctx));
}

void test_setup_bad_request(void) {
    auth_logic_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.state = AUTH_STATE_NEEDS_SETUP;
    TEST_ASSERT_EQUAL(400, web_status_map_setup(-1, &ctx));
}

void test_action_map(void) {
    TEST_ASSERT_EQUAL(200, web_status_map_action_code(HTTP_GUARD_ACTION_ALLOW));
    TEST_ASSERT_EQUAL(401, web_status_map_action_code(HTTP_GUARD_ACTION_401));
    TEST_ASSERT_EQUAL(403, web_status_map_action_code(HTTP_GUARD_ACTION_403_SETUP_REQUIRED));
    TEST_ASSERT_EQUAL(403, web_status_map_action_code(HTTP_GUARD_ACTION_403_ORIGIN));
    TEST_ASSERT_EQUAL(403, web_status_map_action_code(HTTP_GUARD_ACTION_403_HOST));
    TEST_ASSERT_EQUAL(405, web_status_map_action_code(HTTP_GUARD_ACTION_405));
    TEST_ASSERT_EQUAL(409, web_status_map_action_code(HTTP_GUARD_ACTION_409_ALREADY_SETUP));
    TEST_ASSERT_EQUAL(415, web_status_map_action_code(HTTP_GUARD_ACTION_415));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_login_success);
    RUN_TEST(test_login_rate_limited);
    RUN_TEST(test_login_rate_limited_global);
    RUN_TEST(test_login_rate_limited_expired);
    RUN_TEST(test_login_rate_limited_different_ip);
    RUN_TEST(test_login_unauthorized);
    RUN_TEST(test_setup_success);
    RUN_TEST(test_setup_forbidden);
    RUN_TEST(test_setup_bad_request);
    RUN_TEST(test_action_map);
    return UNITY_END();
}
