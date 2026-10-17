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
    config.rate_limit_max_attempts = 3; // Use 3 for faster test
    config.rate_limit_base_lockout_ms = 60000;
    config.rate_limit_max_lockout_ms = 3600000;
    config.rate_limit_global_max_attempts = 15;
    g_ref_random_should_fail = false;
    auth_logic_init(&ctx, &ref_crypto_if, &config);
    auth_logic_setup_password(&ctx, "ValidPassword123!", 17);
}

void tearDown(void) {}

void test_rate_limit_lockout(void) {
    uint8_t token[32];
    uint32_t ip = 12345;

    // Fail 3 times
    TEST_ASSERT_EQUAL(-1, auth_logic_login(&ctx, "WrongPass!", 10, ip, 100, token));
    TEST_ASSERT_EQUAL(-1, auth_logic_login(&ctx, "WrongPass!", 10, ip, 200, token));
    TEST_ASSERT_EQUAL(-1, auth_logic_login(&ctx, "WrongPass!", 10, ip, 300, token)); // This triggers lockout

    // Try correct password while locked out -> should fail
    TEST_ASSERT_EQUAL(-1, auth_logic_login(&ctx, "ValidPassword123!", 17, ip, 400, token));

    // Wait for lockout to expire
    TEST_ASSERT_EQUAL(0, auth_logic_login(&ctx, "ValidPassword123!", 17, ip, 300 + config.rate_limit_base_lockout_ms, token));
}

void test_rate_limit_reset_on_success(void) {
    uint8_t token[32];
    uint32_t ip = 12345;

    // Fail 2 times
    TEST_ASSERT_EQUAL(-1, auth_logic_login(&ctx, "WrongPass!", 10, ip, 100, token));
    TEST_ASSERT_EQUAL(-1, auth_logic_login(&ctx, "WrongPass!", 10, ip, 200, token));

    // Success
    TEST_ASSERT_EQUAL(0, auth_logic_login(&ctx, "ValidPassword123!", 17, ip, 300, token));

    // Fail 2 more times (should not lock out because it was reset)
    TEST_ASSERT_EQUAL(-1, auth_logic_login(&ctx, "WrongPass!", 10, ip, 400, token));
    TEST_ASSERT_EQUAL(-1, auth_logic_login(&ctx, "WrongPass!", 10, ip, 500, token));

    // Still not locked out, correct pass works
    TEST_ASSERT_EQUAL(0, auth_logic_login(&ctx, "ValidPassword123!", 17, ip, 600, token));
}

void test_rate_limit_table_overflow_to_global(void) {
    uint8_t token[32];

    // Fill the table with lockouts for different IPs
    for (int i = 0; i < AUTH_LOGIC_RATE_LIMIT_TABLE_SIZE; i++) {
        uint32_t ip = 1000 + i;
        TEST_ASSERT_EQUAL(-1, auth_logic_login(&ctx, "WrongPass!", 10, ip, 10, token));
        TEST_ASSERT_EQUAL(-1, auth_logic_login(&ctx, "WrongPass!", 10, ip, 10, token));
        TEST_ASSERT_EQUAL(-1, auth_logic_login(&ctx, "WrongPass!", 10, ip, 10, token)); // Locked out
    }

    // Now the table is full of active lockouts.
    // Try a new IP. Should overflow to global.
    uint32_t new_ip = 9999;
    TEST_ASSERT_EQUAL(-1, auth_logic_login(&ctx, "WrongPass!", 10, new_ip, 20, token)); // Global = 1
    TEST_ASSERT_EQUAL(-1, auth_logic_login(&ctx, "WrongPass!", 10, new_ip, 20, token)); // Global = 2
    TEST_ASSERT_EQUAL(-1, auth_logic_login(&ctx, "WrongPass!", 10, new_ip, 20, token)); // Global = 3, global lockout triggered

    // Wait for global lockout to expire (we failed 15 times before, locking out globally)
    // Actually, we failed 3 times globally here since rate_limit_global_max_attempts was 20 in previous mock, let's fix it.

    // Fill the remaining global attempts
    for (int i = 0; i < 15; i++) {
        auth_logic_login(&ctx, "WrongPass!", 10, new_ip, 20, token);
    }

    // Try correct password from a COMPLETELY DIFFERENT IP (should be blocked by global lockout)
    uint32_t innocent_ip = 8888;
    TEST_ASSERT_EQUAL(-1, auth_logic_login(&ctx, "ValidPassword123!", 17, innocent_ip, 30, token));

    // Wait for global lockout to expire
    TEST_ASSERT_EQUAL(0, auth_logic_login(&ctx, "ValidPassword123!", 17, innocent_ip, 20 + config.rate_limit_base_lockout_ms, token));
}

void test_rate_limit_eviction_prevents_unlock(void) {
    uint8_t token[32];
    uint32_t target_ip = 12345;

    // Lock out the target IP
    TEST_ASSERT_EQUAL(-1, auth_logic_login(&ctx, "WrongPass!", 10, target_ip, 100, token));
    TEST_ASSERT_EQUAL(-1, auth_logic_login(&ctx, "WrongPass!", 10, target_ip, 200, token));
    TEST_ASSERT_EQUAL(-1, auth_logic_login(&ctx, "WrongPass!", 10, target_ip, 300, token));

    // Attack: Send requests from many fake IPs to try to evict the target_ip's lockout
    for (int i = 0; i < AUTH_LOGIC_RATE_LIMIT_TABLE_SIZE * 2; i++) {
        uint32_t fake_ip = 50000 + i;
        auth_logic_login(&ctx, "WrongPass!", 10, fake_ip, 400, token);
    }

    // The target IP should STILL be locked out
    TEST_ASSERT_EQUAL(-1, auth_logic_login(&ctx, "ValidPassword123!", 17, target_ip, 500, token));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_rate_limit_lockout);
    RUN_TEST(test_rate_limit_reset_on_success);
    RUN_TEST(test_rate_limit_table_overflow_to_global);
    RUN_TEST(test_rate_limit_eviction_prevents_unlock);
    return UNITY_END();
}
