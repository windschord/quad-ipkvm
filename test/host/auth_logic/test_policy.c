#include "unity.h"
#include "auth_logic.h"
#include "auth_crypto_ref.h"
#include <string.h>

auth_logic_t ctx;
auth_logic_config_t config;

void setUp(void) {
    config.min_password_len = 10;
    config.pbkdf2_iterations = 10000;
    config.session_idle_timeout_ms = 60000;
    config.session_absolute_timeout_ms = 3600000;
    config.rate_limit_window_ms = 60000;
    config.rate_limit_max_attempts = 5;
    config.rate_limit_base_lockout_ms = 60000;
    config.rate_limit_max_lockout_ms = 3600000;
    config.rate_limit_global_max_attempts = 20;
    g_ref_random_should_fail = false;
    auth_logic_init(&ctx, &ref_crypto_if, &config);
}

void tearDown(void) {}

void test_policy_min_length(void) {
    // 9 chars
    TEST_ASSERT_EQUAL(-1, auth_logic_setup_password(&ctx, "123456789", 9));
    TEST_ASSERT_EQUAL(AUTH_STATE_NEEDS_SETUP, ctx.state);
}

void test_policy_max_length(void) {
    char huge_pass[200];
    memset(huge_pass, 'A', sizeof(huge_pass));
    huge_pass[199] = '\0';
    TEST_ASSERT_EQUAL(-1, auth_logic_setup_password(&ctx, huge_pass, 199));
}

void test_policy_weak_passwords(void) {
    TEST_ASSERT_EQUAL(-1, auth_logic_setup_password(&ctx, "1234567890", 10)); // weak list
    TEST_ASSERT_EQUAL(-1, auth_logic_setup_password(&ctx, "admin", 5));      // username identical
    // even padded admin is weak if length doesn't meet
    TEST_ASSERT_EQUAL(-1, auth_logic_setup_password(&ctx, "password", 8));   // weak
}

void test_policy_valid_setup(void) {
    TEST_ASSERT_EQUAL(0, auth_logic_setup_password(&ctx, "ValidPassword123!", 17));
    TEST_ASSERT_EQUAL(AUTH_STATE_READY, ctx.state);
}

void test_fail_closed_random_salt(void) {
    g_ref_random_should_fail = true;
    TEST_ASSERT_EQUAL(-1, auth_logic_setup_password(&ctx, "ValidPassword123!", 17));
    TEST_ASSERT_EQUAL(AUTH_STATE_NEEDS_SETUP, ctx.state);
}

void test_fail_closed_random_token(void) {
    TEST_ASSERT_EQUAL(0, auth_logic_setup_password(&ctx, "ValidPassword123!", 17));

    g_ref_random_should_fail = true;
    uint8_t token[32];
    TEST_ASSERT_EQUAL(-1, auth_logic_login(&ctx, "ValidPassword123!", 17, 1, 0, token));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_policy_min_length);
    RUN_TEST(test_policy_max_length);
    RUN_TEST(test_policy_weak_passwords);
    RUN_TEST(test_policy_valid_setup);
    RUN_TEST(test_fail_closed_random_salt);
    RUN_TEST(test_fail_closed_random_token);
    return UNITY_END();
}
