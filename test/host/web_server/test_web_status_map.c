#include "unity.h"
#include "web_status_map.h"
#include <stdio.h>
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


void test_login_wrong_password_state_ready(void) {
    auth_logic_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.state = AUTH_STATE_READY;
    TEST_ASSERT_EQUAL(401, web_status_map_login(-1, &ctx, 1234, 1000));
}

void test_setup_bad_request_needs_setup(void) {
    auth_logic_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.state = AUTH_STATE_NEEDS_SETUP;
    TEST_ASSERT_EQUAL(400, web_status_map_setup(-1, &ctx));
}

void test_setup_success_needs_setup(void) {
    auth_logic_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.state = AUTH_STATE_NEEDS_SETUP;
    TEST_ASSERT_EQUAL(200, web_status_map_setup(0, &ctx));
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


void test_action_message(void) {
    TEST_ASSERT_NULL(web_status_map_action_message(HTTP_GUARD_ACTION_ALLOW));
    TEST_ASSERT_EQUAL_STRING("unauthorized", web_status_map_action_message(HTTP_GUARD_ACTION_401));
    TEST_ASSERT_EQUAL_STRING("setup_required", web_status_map_action_message(HTTP_GUARD_ACTION_403_SETUP_REQUIRED));
    TEST_ASSERT_EQUAL_STRING("forbidden_origin", web_status_map_action_message(HTTP_GUARD_ACTION_403_ORIGIN));
    TEST_ASSERT_EQUAL_STRING("forbidden_host", web_status_map_action_message(HTTP_GUARD_ACTION_403_HOST));
    TEST_ASSERT_EQUAL_STRING("method_not_allowed", web_status_map_action_message(HTTP_GUARD_ACTION_405));
    TEST_ASSERT_EQUAL_STRING("already_setup", web_status_map_action_message(HTTP_GUARD_ACTION_409_ALREADY_SETUP));
    TEST_ASSERT_EQUAL_STRING("unsupported_media_type", web_status_map_action_message(HTTP_GUARD_ACTION_415));
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

// --- Setup error codes for the password policy -----------------------------------------------

static auth_logic_t needs_setup_ctx(void) {
    auth_logic_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.state = AUTH_STATE_NEEDS_SETUP;
    ctx.config.min_password_len = 10;
    return ctx;
}

void test_password_issue_code_table(void) {
    TEST_ASSERT_NULL(web_status_map_password_issue_code(AUTH_PASSWORD_OK));
    TEST_ASSERT_EQUAL_STRING("password_too_short", web_status_map_password_issue_code(AUTH_PASSWORD_TOO_SHORT));
    TEST_ASSERT_EQUAL_STRING("password_too_long", web_status_map_password_issue_code(AUTH_PASSWORD_TOO_LONG));
    TEST_ASSERT_EQUAL_STRING("password_contains_admin", web_status_map_password_issue_code(AUTH_PASSWORD_CONTAINS_ADMIN));
    TEST_ASSERT_EQUAL_STRING("password_all_same", web_status_map_password_issue_code(AUTH_PASSWORD_ALL_SAME));
    TEST_ASSERT_EQUAL_STRING("password_sequential", web_status_map_password_issue_code(AUTH_PASSWORD_SEQUENTIAL));
    TEST_ASSERT_EQUAL_STRING("password_common", web_status_map_password_issue_code(AUTH_PASSWORD_COMMON));
}

void test_password_issue_codes_are_distinct_and_json_safe(void) {
    const char *codes[AUTH_PASSWORD_COMMON + 1];
    for (int i = AUTH_PASSWORD_TOO_SHORT; i <= AUTH_PASSWORD_COMMON; i++) {
        codes[i] = web_status_map_password_issue_code((auth_password_issue_e)i);
        TEST_ASSERT_NOT_NULL(codes[i]);
        TEST_ASSERT_TRUE(strlen(codes[i]) > 0);
        for (const char *p = codes[i]; *p; p++) {
            TEST_ASSERT_TRUE_MESSAGE((*p >= 'a' && *p <= 'z') || *p == '_', codes[i]);
        }
        TEST_ASSERT_TRUE(strcmp(codes[i], "bad_request") != 0);
        for (int j = AUTH_PASSWORD_TOO_SHORT; j < i; j++) {
            TEST_ASSERT_TRUE_MESSAGE(strcmp(codes[i], codes[j]) != 0, codes[i]);
        }
    }
}

void test_setup_error_code_per_reason(void) {
    auth_logic_t ctx = needs_setup_ctx();
    char long_pwd[200];
    for (size_t i = 0; i < sizeof(long_pwd); i++) long_pwd[i] = (char)('a' + (i * 7) % 26);

    TEST_ASSERT_EQUAL_STRING("password_too_short", web_status_map_setup_error_code(400, &ctx, "short", 5));
    TEST_ASSERT_EQUAL_STRING("password_too_long", web_status_map_setup_error_code(400, &ctx, long_pwd, 129));
    TEST_ASSERT_EQUAL_STRING("password_contains_admin", web_status_map_setup_error_code(400, &ctx, "myAdMiNpass99", 13));
    TEST_ASSERT_EQUAL_STRING("password_all_same", web_status_map_setup_error_code(400, &ctx, "zzzzzzzzzzzz", 12));
    TEST_ASSERT_EQUAL_STRING("password_sequential", web_status_map_setup_error_code(400, &ctx, "abcdefghij", 10));
    TEST_ASSERT_EQUAL_STRING("password_sequential", web_status_map_setup_error_code(400, &ctx, "0123456789", 10));
    TEST_ASSERT_EQUAL_STRING("password_common", web_status_map_setup_error_code(400, &ctx, "1234567890", 10));
    TEST_ASSERT_EQUAL_STRING("password_common", web_status_map_setup_error_code(400, &ctx, "qwertyuiop", 10));
}

void test_setup_error_code_follows_the_configured_minimum(void) {
    auth_logic_t ctx = needs_setup_ctx();
    ctx.config.min_password_len = 14;
    // 12 characters are fine by default, but too short for this configuration.
    TEST_ASSERT_EQUAL_STRING("password_too_short", web_status_map_setup_error_code(400, &ctx, "aZ3$kP9#mQwE", 12));
}

void test_setup_error_code_bad_request_cases(void) {
    auth_logic_t ctx = needs_setup_ctx();
    // The request body could not be parsed (no password available).
    TEST_ASSERT_EQUAL_STRING("bad_request", web_status_map_setup_error_code(400, &ctx, NULL, 0));
    // A 400 although the password passes the policy: some other setup failure.
    TEST_ASSERT_EQUAL_STRING("bad_request", web_status_map_setup_error_code(400, &ctx, "ValidPassword123!", 17));
}

void test_setup_error_code_only_for_400(void) {
    auth_logic_t ctx = needs_setup_ctx();
    const int statuses[] = {0, 200, 201, 301, 401, 403, 409, 413, 429, 500, 503};
    for (size_t i = 0; i < sizeof(statuses) / sizeof(statuses[0]); i++) {
        char msg[32];
        snprintf(msg, sizeof(msg), "status %d", statuses[i]);
        TEST_ASSERT_NULL_MESSAGE(web_status_map_setup_error_code(statuses[i], &ctx, "1234567890", 10), msg);
        TEST_ASSERT_NULL_MESSAGE(web_status_map_setup_error_code(statuses[i], &ctx, NULL, 0), msg);
    }
}

void test_setup_error_code_agrees_with_the_policy(void) {
    auth_logic_t ctx = needs_setup_ctx();
    const char *samples[] = {"", "short", "123456789", "adminXYZ12345", "1111111111", "abcdefghijk",
                             "9876543210", "Password123", "qwertyuiop", "1q2w3e4r5t", "aZ3$kP9#mQ"};
    for (size_t i = 0; i < sizeof(samples) / sizeof(samples[0]); i++) {
        size_t len = strlen(samples[i]);
        auth_password_issue_e issue = auth_logic_password_issue(&ctx, samples[i], len);
        const char *want = web_status_map_password_issue_code(issue);
        if (!want) want = "bad_request";
        TEST_ASSERT_EQUAL_STRING_MESSAGE(want, web_status_map_setup_error_code(400, &ctx, samples[i], len), samples[i]);
    }
}

void test_setup_flow_status_and_code(void) {
    // The real setup path: auth_logic_setup_password() -> status -> error code. Rejections for the policy
    // happen before the crypto layer is used, so no crypto is needed here.
    struct { const char *pwd; const char *code; } cases[] = {
        {"123456789", "password_too_short"},
        {"1234567890", "password_common"},
        {"abcdefghij", "password_sequential"},
        {"1111111111", "password_all_same"},
        {"xxadminxxxxx", "password_contains_admin"},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        auth_logic_t ctx = needs_setup_ctx();
        size_t len = strlen(cases[i].pwd);
        int ret = auth_logic_setup_password(&ctx, cases[i].pwd, len);
        TEST_ASSERT_EQUAL_MESSAGE(-1, ret, cases[i].pwd);
        int status = web_status_map_setup(ret, &ctx);
        TEST_ASSERT_EQUAL_MESSAGE(400, status, cases[i].pwd);
        TEST_ASSERT_EQUAL_STRING_MESSAGE(cases[i].code, web_status_map_setup_error_code(status, &ctx, cases[i].pwd, len), cases[i].pwd);
    }

    // Once the password is set up, a repeated setup is a 403 and has no password error code.
    auth_logic_t ready = needs_setup_ctx();
    ready.state = AUTH_STATE_READY;
    int ret = auth_logic_setup_password(&ready, "1234567890", 10);
    int status = web_status_map_setup(ret, &ready);
    TEST_ASSERT_EQUAL(403, status);
    TEST_ASSERT_NULL(web_status_map_setup_error_code(status, &ready, "1234567890", 10));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_password_issue_code_table);
    RUN_TEST(test_password_issue_codes_are_distinct_and_json_safe);
    RUN_TEST(test_setup_error_code_per_reason);
    RUN_TEST(test_setup_error_code_follows_the_configured_minimum);
    RUN_TEST(test_setup_error_code_bad_request_cases);
    RUN_TEST(test_setup_error_code_only_for_400);
    RUN_TEST(test_setup_error_code_agrees_with_the_policy);
    RUN_TEST(test_setup_flow_status_and_code);
    RUN_TEST(test_login_success);
    RUN_TEST(test_login_rate_limited);
    RUN_TEST(test_login_rate_limited_global);
    RUN_TEST(test_login_rate_limited_expired);
    RUN_TEST(test_login_rate_limited_different_ip);
    RUN_TEST(test_login_unauthorized);
    RUN_TEST(test_login_wrong_password_state_ready);
    RUN_TEST(test_setup_bad_request_needs_setup);
    RUN_TEST(test_setup_success_needs_setup);
    RUN_TEST(test_setup_success);
    RUN_TEST(test_setup_forbidden);
    RUN_TEST(test_setup_bad_request);
    RUN_TEST(test_action_message);
    RUN_TEST(test_action_map);
    return UNITY_END();
}
