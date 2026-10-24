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

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_login_success);
    RUN_TEST(test_login_rate_limited);
    RUN_TEST(test_login_unauthorized);
    RUN_TEST(test_setup_success);
    RUN_TEST(test_setup_forbidden);
    RUN_TEST(test_setup_bad_request);
    return UNITY_END();
}
