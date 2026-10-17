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
    config.rate_limit_max_attempts = 5;
    config.rate_limit_lockout_ms = 60000;
    g_ref_random_should_fail = false;
    auth_logic_init(&ctx, &ref_crypto_if, &config);
}

void tearDown(void) {}

void test_needs_setup_blocks_login(void) {
    uint8_t token[32];
    TEST_ASSERT_EQUAL(AUTH_STATE_NEEDS_SETUP, ctx.state);
    TEST_ASSERT_EQUAL(-1, auth_logic_login(&ctx, "ValidPassword123!", 17, 1, 0, token));
}

void test_double_setup_prevented(void) {
    TEST_ASSERT_EQUAL(0, auth_logic_setup_password(&ctx, "ValidPassword123!", 17));
    TEST_ASSERT_EQUAL(AUTH_STATE_READY, ctx.state);

    // Try to setup again
    TEST_ASSERT_EQUAL(-1, auth_logic_setup_password(&ctx, "AnotherPassword!", 16));
}

void test_login_success_and_validate(void) {
    TEST_ASSERT_EQUAL(0, auth_logic_setup_password(&ctx, "ValidPassword123!", 17));

    uint8_t token[32];
    TEST_ASSERT_EQUAL(0, auth_logic_login(&ctx, "ValidPassword123!", 17, 1, 0, token));

    // Validate
    TEST_ASSERT_EQUAL(0, auth_logic_validate_session(&ctx, token, 32, 1000));
}

void test_session_idle_timeout(void) {
    TEST_ASSERT_EQUAL(0, auth_logic_setup_password(&ctx, "ValidPassword123!", 17));

    uint8_t token[32];
    TEST_ASSERT_EQUAL(0, auth_logic_login(&ctx, "ValidPassword123!", 17, 1, 0, token));

    // Validate just before timeout
    TEST_ASSERT_EQUAL(0, auth_logic_validate_session(&ctx, token, 32, config.session_idle_timeout_ms));

    // Idle out
    TEST_ASSERT_EQUAL(-1, auth_logic_validate_session(&ctx, token, 32, config.session_idle_timeout_ms * 2 + 1000));
}

void test_session_absolute_timeout(void) {
    TEST_ASSERT_EQUAL(0, auth_logic_setup_password(&ctx, "ValidPassword123!", 17));

    uint8_t token[32];
    TEST_ASSERT_EQUAL(0, auth_logic_login(&ctx, "ValidPassword123!", 17, 1, 0, token)); // Created at T=0

    // Validate keeps idle timeout fresh. We advance the time by a little less than the idle timeout.
    TEST_ASSERT_EQUAL(0, auth_logic_validate_session(&ctx, token, 32, config.session_idle_timeout_ms - 1000));

    // Reach absolute timeout
    TEST_ASSERT_EQUAL(-1, auth_logic_validate_session(&ctx, token, 32, config.session_absolute_timeout_ms + 1000));
}

void test_logout(void) {
    TEST_ASSERT_EQUAL(0, auth_logic_setup_password(&ctx, "ValidPassword123!", 17));

    uint8_t token[32];
    TEST_ASSERT_EQUAL(0, auth_logic_login(&ctx, "ValidPassword123!", 17, 1, 0, token));

    TEST_ASSERT_EQUAL(0, auth_logic_logout(&ctx, token, 32));
    TEST_ASSERT_EQUAL(-1, auth_logic_validate_session(&ctx, token, 32, 0));
}

void test_logout_all(void) {
    TEST_ASSERT_EQUAL(0, auth_logic_setup_password(&ctx, "ValidPassword123!", 17));

    uint8_t t1[32], t2[32];
    TEST_ASSERT_EQUAL(0, auth_logic_login(&ctx, "ValidPassword123!", 17, 1, 0, t1));
    TEST_ASSERT_EQUAL(0, auth_logic_login(&ctx, "ValidPassword123!", 17, 1, 0, t2));

    auth_logic_logout_all(&ctx);

    TEST_ASSERT_EQUAL(-1, auth_logic_validate_session(&ctx, t1, 32, 0));
    TEST_ASSERT_EQUAL(-1, auth_logic_validate_session(&ctx, t2, 32, 0));
}

void test_max_sessions_eviction(void) {
    TEST_ASSERT_EQUAL(0, auth_logic_setup_password(&ctx, "ValidPassword123!", 17));

    uint8_t tokens[AUTH_LOGIC_SESSION_MAX + 1][32];

    for (int i = 0; i < AUTH_LOGIC_SESSION_MAX; i++) {
        TEST_ASSERT_EQUAL(0, auth_logic_login(&ctx, "ValidPassword123!", 17, 1, i*10, tokens[i]));
    }

    // One more should evict the oldest (tokens[0])
    TEST_ASSERT_EQUAL(0, auth_logic_login(&ctx, "ValidPassword123!", 17, 1, 100, tokens[AUTH_LOGIC_SESSION_MAX]));

    TEST_ASSERT_EQUAL(-1, auth_logic_validate_session(&ctx, tokens[0], 32, 100)); // Evicted
    TEST_ASSERT_EQUAL(0, auth_logic_validate_session(&ctx, tokens[1], 32, 100));  // Still valid
    TEST_ASSERT_EQUAL(0, auth_logic_validate_session(&ctx, tokens[AUTH_LOGIC_SESSION_MAX], 32, 100)); // New one valid
}

void test_serialize_deserialize(void) {
    TEST_ASSERT_EQUAL(0, auth_logic_setup_password(&ctx, "ValidPassword123!", 17));

    uint8_t ser[256];
    size_t len;
    TEST_ASSERT_EQUAL(0, auth_logic_serialize_state(&ctx, ser, sizeof(ser), &len));
    TEST_ASSERT_EQUAL(AUTH_LOGIC_SERIALIZED_LEN, len);

    auth_logic_t ctx2;
    auth_logic_init(&ctx2, &ref_crypto_if, &config);
    TEST_ASSERT_EQUAL(AUTH_STATE_NEEDS_SETUP, ctx2.state);

    TEST_ASSERT_EQUAL(0, auth_logic_load_state(&ctx2, ser, len));
    TEST_ASSERT_EQUAL(AUTH_STATE_READY, ctx2.state);

    // Can login on new ctx
    uint8_t token[32];
    TEST_ASSERT_EQUAL(0, auth_logic_login(&ctx2, "ValidPassword123!", 17, 1, 0, token));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_needs_setup_blocks_login);
    RUN_TEST(test_double_setup_prevented);
    RUN_TEST(test_login_success_and_validate);
    RUN_TEST(test_session_idle_timeout);
    RUN_TEST(test_session_absolute_timeout);
    RUN_TEST(test_logout);
    RUN_TEST(test_logout_all);
    RUN_TEST(test_max_sessions_eviction);
    RUN_TEST(test_serialize_deserialize);
    return UNITY_END();
}
