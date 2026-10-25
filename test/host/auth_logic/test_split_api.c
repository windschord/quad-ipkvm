#include "unity.h"
#include "auth_logic.h"
#include "auth_crypto_ref.h"
#include <string.h>

static auth_logic_t ctx_new;
static auth_logic_config_t config;

void setUp(void) {
    g_ref_random_should_fail = false;
    auth_logic_config_default(&config);
    config.pbkdf2_iterations = 10000;

    auth_logic_init(&ctx_new, &ref_crypto_if, &config);
    // Force to needs_setup if we want to test setup.
    ctx_new.state = AUTH_STATE_NEEDS_SETUP;
}

void tearDown(void) {
    g_ref_random_should_fail = false;
}

void test_login_toctou(void) {
    TEST_ASSERT_EQUAL(0, auth_logic_setup_password(&ctx_new, "strongpassword123!", 18));

    auth_login_job_t job;
    TEST_ASSERT_EQUAL(0, auth_logic_login_begin(&ctx_new, 12345, 1000, &job));

    int match = auth_logic_login_compute(ctx_new.crypto, &job, "strongpassword123!", 18);
    TEST_ASSERT_EQUAL(1, match);

    // Simulate another change_password while compute was happening
    TEST_ASSERT_EQUAL(0, auth_logic_change_password(&ctx_new, "strongpassword123!", 18, "newstrongpassword123!", 21, 12345, 1500));

    // Let's reset the hash and salt to match the original job so that ONLY generation check fails.
    memcpy(ctx_new.salt, job.salt, AUTH_LOGIC_SALT_LEN);
    memcpy(ctx_new.hash, job.expected_hash, AUTH_LOGIC_HASH_LEN);

    // Finish should now fail due to generation mismatch
    uint8_t token[AUTH_LOGIC_TOKEN_LEN];
    int ret = auth_logic_login_finish(&ctx_new, &job, match, 12345, 2000, token);
    TEST_ASSERT_EQUAL(-1, ret);
}

void test_setup_toctou(void) {
    auth_setup_job_t job;
    TEST_ASSERT_EQUAL(0, auth_logic_setup_begin(&ctx_new, "strongpassword123!", 18, &job));
    TEST_ASSERT_EQUAL(0, auth_logic_setup_compute(ctx_new.crypto, &job, "strongpassword123!", 18));

    // Simulate another setup finishing first
    TEST_ASSERT_EQUAL(0, auth_logic_setup_password(&ctx_new, "strongpassword123!", 18));

    // Fix the state back to NEEDS_SETUP so ONLY generation fails
    ctx_new.state = AUTH_STATE_NEEDS_SETUP;
    // Finish should fail because generation mismatch
    TEST_ASSERT_EQUAL(-1, auth_logic_setup_finish(&ctx_new, &job));
}

void test_login_finish_respects_match(void) {
    TEST_ASSERT_EQUAL(0, auth_logic_setup_password(&ctx_new, "strongpassword123!", 18));

    auth_login_job_t job;
    TEST_ASSERT_EQUAL(0, auth_logic_login_begin(&ctx_new, 12345, 1000, &job));

    // match is 0 here
    uint8_t token[AUTH_LOGIC_TOKEN_LEN];
    int ret = auth_logic_login_finish(&ctx_new, &job, 0, 12345, 2000, token);
    TEST_ASSERT_EQUAL(-1, ret);
}

void test_setup_finish_respects_state(void) {
    auth_setup_job_t job;
    TEST_ASSERT_EQUAL(0, auth_logic_setup_begin(&ctx_new, "strongpassword123!", 18, &job));
    TEST_ASSERT_EQUAL(0, auth_logic_setup_compute(ctx_new.crypto, &job, "strongpassword123!", 18));

    // force state to READY manually (simulate a bug or some concurrent non-setup transition)
    // Actually the easiest way is to use the existing setup, but fix generation
    // Instead we can just do:
    TEST_ASSERT_EQUAL(0, auth_logic_setup_password(&ctx_new, "strongpassword123!", 18));
    // To only test state, let's reset generation so generation check passes but state fails
    ctx_new.generation = job.generation;

    int ret = auth_logic_setup_finish(&ctx_new, &job);
    TEST_ASSERT_EQUAL(-1, ret);
}

void test_compute_signature(void) {
    int (*login_fn)(const auth_crypto_if_t *, const auth_login_job_t *, const char *, size_t) = auth_logic_login_compute;
    TEST_ASSERT_NOT_NULL(login_fn);

    int (*setup_fn)(const auth_crypto_if_t *, auth_setup_job_t *, const char *, size_t) = auth_logic_setup_compute;
    TEST_ASSERT_NOT_NULL(setup_fn);
}

void test_no_password_in_job(void) {
    TEST_ASSERT_EQUAL(0, auth_logic_setup_password(&ctx_new, "strongpassword123!", 18));

    auth_login_job_t job;
    auth_logic_login_begin(&ctx_new, 12345, 1000, &job);

    const char *pwd = "strongpassword123!";
    size_t pwd_len = strlen(pwd);
    uint8_t *job_bytes = (uint8_t*)&job;
    int found = 0;
    for (size_t i = 0; i <= sizeof(job) - pwd_len; i++) {
        if (memcmp(&job_bytes[i], pwd, pwd_len) == 0) {
            found = 1;
            break;
        }
    }
    TEST_ASSERT_EQUAL(0, found);
}


void test_setup_finish_protects_against_compute_failure(void) {
    auth_setup_job_t job;
    TEST_ASSERT_EQUAL(0, auth_logic_setup_begin(&ctx_new, "strongpassword123!", 18, &job));

    // Simulate compute failure by zeroing out the hash manually, simulating what happens if pbkdf2 fails.
    memset(job.computed_hash, 0, AUTH_LOGIC_HASH_LEN);

    // Finish should fail and not transition to READY.
    TEST_ASSERT_EQUAL(-1, auth_logic_setup_finish(&ctx_new, &job));
    TEST_ASSERT_EQUAL(AUTH_STATE_NEEDS_SETUP, ctx_new.state);
}

void test_policy_in_setup_begin(void) {
    auth_setup_job_t job;
    // test short password
    TEST_ASSERT_EQUAL(-1, auth_logic_setup_begin(&ctx_new, "short", 5, &job));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_login_toctou);
    RUN_TEST(test_setup_toctou);
    RUN_TEST(test_login_finish_respects_match);
    RUN_TEST(test_setup_finish_respects_state);
    RUN_TEST(test_compute_signature);
    RUN_TEST(test_no_password_in_job);
    RUN_TEST(test_setup_finish_protects_against_compute_failure);
    RUN_TEST(test_policy_in_setup_begin);
    return UNITY_END();
}
