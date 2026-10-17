#include "unity.h"
#include "auth_logic.h"
#include "auth_crypto_ref.h"
#include <string.h>

// Simple deterministic RNG for fuzzing
static uint32_t fuzz_seed = 123456789;
static uint32_t xorshift32(void) {
    fuzz_seed ^= fuzz_seed << 13;
    fuzz_seed ^= fuzz_seed >> 17;
    fuzz_seed ^= fuzz_seed << 5;
    return fuzz_seed;
}

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
    fuzz_seed = 123456789; // Reset seed
}

void tearDown(void) {}

#define MAX_TRACKED_SESSIONS 100
typedef struct {
    uint8_t token[32];
    bool valid;
    uint64_t created_ms;
    uint64_t last_used_ms;
} tracked_session_t;

tracked_session_t tracked[MAX_TRACKED_SESSIONS];
int num_tracked = 0;

void test_fuzz_session_invariants(void) {
    TEST_ASSERT_EQUAL(0, auth_logic_setup_password(&ctx, "ValidPassword123!", 17));

    uint64_t now_ms = 0;

    for (int step = 0; step < 1000; step++) {
        // Advance time randomly between 0 and 10 seconds
        now_ms += xorshift32() % 10000;

        int action = xorshift32() % 4;

        if (action == 0 && num_tracked < MAX_TRACKED_SESSIONS) {
            // Login
            uint8_t token[32];
            int ret = auth_logic_login(&ctx, "ValidPassword123!", 17, 1, now_ms, token);
            if (ret == 0) {
                memcpy(tracked[num_tracked].token, token, 32);
                tracked[num_tracked].valid = true;
                tracked[num_tracked].created_ms = now_ms;
                tracked[num_tracked].last_used_ms = now_ms;
                num_tracked++;
            }
        } else if (action == 1 && num_tracked > 0) {
            // Validate a random tracked session
            int idx = xorshift32() % num_tracked;
            int ret = auth_logic_validate_session(&ctx, tracked[idx].token, 32, now_ms);

            // INDEPENDENT EXPECTATION CALCULATION:
            bool expected_valid = tracked[idx].valid;
            if (expected_valid) {
                if (now_ms - tracked[idx].last_used_ms > config.session_idle_timeout_ms ||
                    now_ms - tracked[idx].created_ms > config.session_absolute_timeout_ms) {
                    expected_valid = false;
                    tracked[idx].valid = false; // It expired
                }
            }

            // Note: because of max sessions eviction, a token might be valid in our tracker but evicted in ctx.
            // So if ret == -1 but expected_valid is true, it MUST be because it was evicted due to MAX_SESSION.
            // We can't strictly assert ret == 0 if expected_valid is true without tracking eviction perfectly,
            // but we CAN assert that if ret == 0, expected_valid MUST be true (no expired/logged out tokens are accepted).
            if (ret == 0) {
                TEST_ASSERT_TRUE(expected_valid);
                tracked[idx].last_used_ms = now_ms; // Update tracker
            }
        } else if (action == 2 && num_tracked > 0) {
            // Logout a random session
            int idx = xorshift32() % num_tracked;
            auth_logic_logout(&ctx, tracked[idx].token, 32);
            tracked[idx].valid = false;
        } else if (action == 3) {
            // Logout all
            auth_logic_logout_all(&ctx);
            for (int i = 0; i < num_tracked; i++) {
                tracked[i].valid = false;
            }
        }

        // Invariant: NEVER more than SESSION_MAX active sessions
        int active_count = 0;
        for (int i = 0; i < AUTH_LOGIC_SESSION_MAX; i++) {
            if (ctx.sessions[i].is_active) active_count++;
        }
        TEST_ASSERT_LESS_OR_EQUAL(AUTH_LOGIC_SESSION_MAX, active_count);
    }
}

void test_time_wraparound(void) {
    TEST_ASSERT_EQUAL(0, auth_logic_setup_password(&ctx, "ValidPassword123!", 17));

    uint64_t huge_time = UINT64_MAX - 50000; // Close to wraparound
    uint8_t token[32];

    TEST_ASSERT_EQUAL(0, auth_logic_login(&ctx, "ValidPassword123!", 17, 1, huge_time, token));

    // Validate just before wrap
    TEST_ASSERT_EQUAL(0, auth_logic_validate_session(&ctx, token, 32, huge_time + 1000));

    // Wrap around
    uint64_t wrapped_time = huge_time + 100000; // Will be small positive number due to overflow

    // Since we use subtraction (now_ms - created_ms), unsigned arithmetic handles wraparound correctly!
    // However, if the time delta is very large (e.g. timeout happened during wrap), it should fail.
    TEST_ASSERT_EQUAL(-1, auth_logic_validate_session(&ctx, token, 32, wrapped_time));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_fuzz_session_invariants);
    RUN_TEST(test_time_wraparound);
    return UNITY_END();
}
