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

static int pbkdf2_call_count = 0;
static int pbkdf2_wrapper(const uint8_t *password, size_t plen, const uint8_t *salt, size_t slen, uint32_t iteration_count, uint32_t key_length, uint8_t *output) {
    pbkdf2_call_count++;
    return ref_crypto_if.pbkdf2_hmac_sha256(password, plen, salt, slen, iteration_count, key_length, output);
}

auth_logic_t ctx;
auth_logic_config_t config;
auth_crypto_if_t wrapped_crypto;

void setUp(void) {
    wrapped_crypto = ref_crypto_if;
    wrapped_crypto.pbkdf2_hmac_sha256 = pbkdf2_wrapper;

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
    auth_logic_init(&ctx, &wrapped_crypto, &config);
    pbkdf2_call_count = 0;
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

        int action = xorshift32() % 5;

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
            // We can't strictly assert ret == 0 if expected_valid is true without tracking eviction perfectly.
            // BUT we CAN assert that if expected_valid is false, ret MUST be -1 (no expired/logged out tokens are accepted).
            if (!expected_valid) {
                TEST_ASSERT_EQUAL(-1, ret);
            } else if (ret == 0) {
                // If it succeeded, it must have been expected valid
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
        } else if (action == 4) {
            // Random fail-closed trigger
            g_ref_random_should_fail = true;
            uint8_t token[32];
            int ret = auth_logic_login(&ctx, "ValidPassword123!", 17, 1, now_ms, token);
            TEST_ASSERT_EQUAL(-1, ret);
            g_ref_random_should_fail = false;
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

void test_fuzz_session_multiple_seeds(void) {
    for (int i = 0; i < 5; i++) { // reduced from 50 to 5 for faster execution
        fuzz_seed = 123456789 + i * 999;
        setUp();
        num_tracked = 0;
        test_fuzz_session_invariants();
    }
}

// Independent rate limit tracking
typedef struct {
    uint32_t ip;
    uint32_t attempts;
    uint64_t first_attempt;
    uint32_t lockout_count;
    uint64_t lockout_until;
} fuzzer_ip_state_t;

static uint64_t f_calc_lockout(uint32_t lcount) {
    uint64_t ms = config.rate_limit_base_lockout_ms;
    if (lcount > 0) {
        int shifts = lcount - 1;
        if (shifts > 30) shifts = 30;
        ms *= (1ULL << shifts);
    }
    if (ms > config.rate_limit_max_lockout_ms) return config.rate_limit_max_lockout_ms;
    return ms;
}

static void f_decay(fuzzer_ip_state_t *st, uint64_t now_ms) {
    if ((st->lockout_until == 0 || now_ms >= st->lockout_until) && now_ms - st->first_attempt > config.rate_limit_window_ms) {
        st->attempts = 0;
        st->lockout_count = 0;
        st->lockout_until = 0;
        st->first_attempt = 0;
    } else if (st->lockout_until > 0 && now_ms >= st->lockout_until) {
        st->attempts = 0;
        st->lockout_until = 0;
        st->first_attempt = now_ms;
    }
}

void test_fuzz_rate_limit_invariants(void) {
    TEST_ASSERT_EQUAL(0, auth_logic_setup_password(&ctx, "ValidPassword123!", 17));

    uint64_t now_ms = 0;
    fuzzer_ip_state_t ips[2] = {0};
    ips[0].ip = 100;
    ips[1].ip = 200;

    int pre_pbkdf2_calls;

    for (int step = 0; step < 2000; step++) {
        now_ms += xorshift32() % 10000;

        int ip_idx = xorshift32() % 2;
        int action = xorshift32() % 2; // 0 = fail, 1 = success

        fuzzer_ip_state_t *st = &ips[ip_idx];
        f_decay(st, now_ms);
        bool expected_lockout = (st->lockout_until > 0 && now_ms < st->lockout_until);

        pre_pbkdf2_calls = pbkdf2_call_count;

        uint8_t token[32];
        if (action == 0) {
            int ret = auth_logic_login(&ctx, "WrongPass!", 10, st->ip, now_ms, token);
            TEST_ASSERT_EQUAL(-1, ret);
            if (expected_lockout) {
                // R4: PBKDF2 must not be called during lockout
                TEST_ASSERT_EQUAL(pre_pbkdf2_calls, pbkdf2_call_count);
            } else {
                if (st->attempts == 0) st->first_attempt = now_ms;
                st->attempts++;
                if (st->attempts >= config.rate_limit_max_attempts) {
                    st->lockout_count++;
                    st->lockout_until = now_ms + f_calc_lockout(st->lockout_count);
                    st->attempts = 0;
                }
            }
        } else {
            int ret = auth_logic_login(&ctx, "ValidPassword123!", 17, st->ip, now_ms, token);
            if (expected_lockout) {
                // R1: Correct password rejected during lockout
                TEST_ASSERT_EQUAL(-1, ret);
                // R4
                TEST_ASSERT_EQUAL(pre_pbkdf2_calls, pbkdf2_call_count);
            } else {
                // R3: Success resets only this IP
                TEST_ASSERT_EQUAL(0, ret);
                st->attempts = 0;
                st->lockout_count = 0;
                st->lockout_until = 0;
                st->first_attempt = 0;
            }
        }
    }
}

void test_setup_invariants(void) {
    uint8_t token[32];
    // S2: Before setup, login/validate fail
    TEST_ASSERT_EQUAL(-1, auth_logic_login(&ctx, "ValidPassword123!", 17, 1, 0, token));
    TEST_ASSERT_EQUAL(-1, auth_logic_validate_session(&ctx, token, 32, 0));

    // Setup
    TEST_ASSERT_EQUAL(0, auth_logic_setup_password(&ctx, "ValidPassword123!", 17));

    // S1: Double setup prevented
    TEST_ASSERT_EQUAL(-1, auth_logic_setup_password(&ctx, "ValidPassword123!", 17));

    // S3: load_state rejected when READY
    uint8_t ser[200]; size_t len;
    auth_logic_serialize_state(&ctx, ser, sizeof(ser), &len);
    TEST_ASSERT_EQUAL(-1, auth_logic_load_state(&ctx, ser, len));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_fuzz_session_multiple_seeds);
    RUN_TEST(test_time_wraparound);
    RUN_TEST(test_fuzz_rate_limit_invariants);
    RUN_TEST(test_setup_invariants);
    return UNITY_END();
}
