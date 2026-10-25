#include "unity.h"
#include "auth_logic.h"
#include "auth_crypto_ref.h"
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

// ---------------------------------------------------------------------------------------------
// Reference copy of the password policy as it was BEFORE auth_logic_password_issue() existed
// (components/auth_logic/src/auth_logic.c at de89316, lines 17-76, only the identifiers renamed).
// The tests below require auth_logic_password_issue() == AUTH_PASSWORD_OK to agree exactly with
// "length is acceptable and this reference says the password is not weak".
// Verify the copy with:
// (one line each; no trailing backslashes, which would continue the comment and trip -Wcomment)
//   git show de89316:components/auth_logic/src/auth_logic.c | sed -n '17,76p' | sed 's/to_lower/ref_to_lower/g; s/is_weak_password/ref_is_weak_password/g' > orig.txt
//   sed -n '/^\/\/ BEGIN REFERENCE COPY$/,/^\/\/ END REFERENCE COPY$/p' <this file> | sed '1d;$d' > copy.txt && diff orig.txt copy.txt
// ---------------------------------------------------------------------------------------------
// BEGIN REFERENCE COPY
static char ref_to_lower(char c) {
    if (c >= 'A' && c <= 'Z') return c + ('a' - 'A');
    return c;
}

static bool ref_is_weak_password(const char *password, size_t len) {
    if (len < AUTH_LOGIC_MIN_PASSWORD_LEN) return true;

    // Check if it contains username (case insensitive)
    for (size_t i = 0; i <= len - 5; i++) {
        if (ref_to_lower(password[i]) == 'a' &&
            ref_to_lower(password[i+1]) == 'd' &&
            ref_to_lower(password[i+2]) == 'm' &&
            ref_to_lower(password[i+3]) == 'i' &&
            ref_to_lower(password[i+4]) == 'n') {
            return true;
        }
    }

    // Check for all identical characters
    bool all_same = true;
    for (size_t i = 1; i < len; i++) {
        if (password[i] != password[0]) {
            all_same = false;
            break;
        }
    }
    if (all_same) return true;

    // Check for sequential characters (up or down)
    bool seq_up = true;
    bool seq_down = true;
    for (size_t i = 1; i < len; i++) {
        if (password[i] != password[i-1] + 1) seq_up = false;
        if (password[i] != password[i-1] - 1) seq_down = false;
    }
    if (seq_up || seq_down) return true;

    // Common weak passwords (case-insensitive checking, >= 10 chars)
    const char* weak_list[] = {
        "password123", "password1234", "password12345", "password123456",
        "qwertyuiop", "1234567890", "0987654321", "1111111111",
        "administrator", "admin12345", "admin123456", "adminadmin",
        "letmein123", "sunshine123", "iloveyou12",
        "welcome123", "1q2w3e4r5t", "1qaz2wsx3edc",
        "passwordpassword", "changeme123", "default123"
    };

    char pwd_lower[AUTH_LOGIC_MAX_PASSWORD_LEN + 1];
    for (size_t i = 0; i < len; i++) {
        pwd_lower[i] = ref_to_lower(password[i]);
    }
    pwd_lower[len] = '\0';

    for (size_t i = 0; i < sizeof(weak_list)/sizeof(weak_list[0]); i++) {
        if (strcmp(pwd_lower, weak_list[i]) == 0) return true;
    }

    return false;
}
// END REFERENCE COPY

// The same list, kept separately so the tests can build "common password" samples.
static const char *const k_common[] = {
    "password123", "password1234", "password12345", "password123456",
    "qwertyuiop", "1234567890", "0987654321", "1111111111",
    "administrator", "admin12345", "admin123456", "adminadmin",
    "letmein123", "sunshine123", "iloveyou12",
    "welcome123", "1q2w3e4r5t", "1qaz2wsx3edc",
    "passwordpassword", "changeme123", "default123"
};
#define K_COMMON_COUNT (sizeof(k_common) / sizeof(k_common[0]))

static auth_logic_t make_ctx(uint32_t min_len) {
    auth_logic_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.state = AUTH_STATE_NEEDS_SETUP;
    ctx.config.min_password_len = min_len;
    return ctx;
}

static auth_password_issue_e issue_of(const char *pwd) {
    auth_logic_t ctx = make_ctx(10);
    return auth_logic_password_issue(&ctx, pwd, strlen(pwd));
}

// --- Individual rules ---------------------------------------------------------------------

void test_acceptable_passwords(void) {
    TEST_ASSERT_EQUAL(AUTH_PASSWORD_OK, issue_of("ValidPassword123!"));
    TEST_ASSERT_EQUAL(AUTH_PASSWORD_OK, issue_of("aZ3$kP9#mQ"));          // exactly 10
    TEST_ASSERT_EQUAL(AUTH_PASSWORD_OK, issue_of("1234567891"));          // looks like a run, but 9 -> 1 breaks it
    TEST_ASSERT_EQUAL(AUTH_PASSWORD_OK, issue_of("abcdefghiX"));          // run broken by the last character
}

void test_too_short(void) {
    TEST_ASSERT_EQUAL(AUTH_PASSWORD_TOO_SHORT, issue_of(""));
    TEST_ASSERT_EQUAL(AUTH_PASSWORD_TOO_SHORT, issue_of("123456789"));    // 9
    TEST_ASSERT_EQUAL(AUTH_PASSWORD_TOO_SHORT, issue_of("aZ3$kP9#m"));    // 9, otherwise fine
    TEST_ASSERT_EQUAL(AUTH_PASSWORD_TOO_SHORT, issue_of("admin"));
}

void test_too_long(void) {
    char buf[300];
    // 129 and 200 bytes of a varied, otherwise acceptable pattern
    for (size_t i = 0; i < sizeof(buf) - 1; i++) buf[i] = (char)('a' + (i * 7) % 26);
    auth_logic_t ctx = make_ctx(10);
    TEST_ASSERT_EQUAL(AUTH_PASSWORD_OK, auth_logic_password_issue(&ctx, buf, 128));
    TEST_ASSERT_EQUAL(AUTH_PASSWORD_TOO_LONG, auth_logic_password_issue(&ctx, buf, 129));
    TEST_ASSERT_EQUAL(AUTH_PASSWORD_TOO_LONG, auth_logic_password_issue(&ctx, buf, 200));
}

void test_contains_admin(void) {
    TEST_ASSERT_EQUAL(AUTH_PASSWORD_CONTAINS_ADMIN, issue_of("xxxadminxxxxx"));
    TEST_ASSERT_EQUAL(AUTH_PASSWORD_CONTAINS_ADMIN, issue_of("myAdMiNpass99"));      // case-insensitive
    TEST_ASSERT_EQUAL(AUTH_PASSWORD_CONTAINS_ADMIN, issue_of("ADMINxyz12345"));      // at the start
    TEST_ASSERT_EQUAL(AUTH_PASSWORD_CONTAINS_ADMIN, issue_of("9876xyz12admin"));     // at the end
    TEST_ASSERT_EQUAL(AUTH_PASSWORD_OK, issue_of("admi-nXyz12345"));                 // not contiguous
}

void test_all_same(void) {
    TEST_ASSERT_EQUAL(AUTH_PASSWORD_ALL_SAME, issue_of("zzzzzzzzzzzz"));
    TEST_ASSERT_EQUAL(AUTH_PASSWORD_ALL_SAME, issue_of("0000000000"));
    TEST_ASSERT_EQUAL(AUTH_PASSWORD_OK, issue_of("zzzzzzzzzzzy"));
}

void test_sequential(void) {
    // A run is judged on the character codes: every character is the previous one +1 (or -1).
    TEST_ASSERT_EQUAL(AUTH_PASSWORD_SEQUENTIAL, issue_of("0123456789"));   // digits up
    TEST_ASSERT_EQUAL(AUTH_PASSWORD_SEQUENTIAL, issue_of("9876543210"));   // digits down
    TEST_ASSERT_EQUAL(AUTH_PASSWORD_SEQUENTIAL, issue_of("abcdefghij"));   // letters up
    TEST_ASSERT_EQUAL(AUTH_PASSWORD_SEQUENTIAL, issue_of("jihgfedcba"));   // letters down
    TEST_ASSERT_EQUAL(AUTH_PASSWORD_SEQUENTIAL, issue_of("ABCDEFGHIJKL")); // upper case, 12
}

void test_one_to_zero_is_not_a_run(void) {
    // "1234567890" and "0987654321" are NOT runs ('9' -> '0' is not +1): the common-password list rejects them.
    TEST_ASSERT_EQUAL(AUTH_PASSWORD_COMMON, issue_of("1234567890"));
    TEST_ASSERT_EQUAL(AUTH_PASSWORD_COMMON, issue_of("0987654321"));
}

void test_common_every_list_entry(void) {
    // Expected reason per entry of the common list, derived by hand (higher-priority rules win).
    static const auth_password_issue_e expected[K_COMMON_COUNT] = {
        AUTH_PASSWORD_COMMON, AUTH_PASSWORD_COMMON, AUTH_PASSWORD_COMMON, AUTH_PASSWORD_COMMON, // password...
        AUTH_PASSWORD_COMMON,                                                                 // qwertyuiop
        AUTH_PASSWORD_COMMON, AUTH_PASSWORD_COMMON,                                           // 1234567890, 0987654321 (not runs)
        AUTH_PASSWORD_ALL_SAME,                                                               // 1111111111
        AUTH_PASSWORD_CONTAINS_ADMIN, AUTH_PASSWORD_CONTAINS_ADMIN,                           // administrator, admin12345
        AUTH_PASSWORD_CONTAINS_ADMIN, AUTH_PASSWORD_CONTAINS_ADMIN,                           // admin123456, adminadmin
        AUTH_PASSWORD_COMMON, AUTH_PASSWORD_COMMON, AUTH_PASSWORD_COMMON,                     // letmein123, sunshine123, iloveyou12
        AUTH_PASSWORD_COMMON, AUTH_PASSWORD_COMMON, AUTH_PASSWORD_COMMON,                     // welcome123, 1q2w3e4r5t, 1qaz2wsx3edc
        AUTH_PASSWORD_COMMON, AUTH_PASSWORD_COMMON, AUTH_PASSWORD_COMMON                      // passwordpassword, changeme123, default123
    };
    for (size_t i = 0; i < K_COMMON_COUNT; i++) {
        char msg[64];
        snprintf(msg, sizeof(msg), "common list entry %zu: %s", i, k_common[i]);
        TEST_ASSERT_EQUAL_INT_MESSAGE(expected[i], issue_of(k_common[i]), msg);
    }
}

void test_common_is_case_insensitive(void) {
    TEST_ASSERT_EQUAL(AUTH_PASSWORD_COMMON, issue_of("PassWord123"));
    TEST_ASSERT_EQUAL(AUTH_PASSWORD_COMMON, issue_of("QWERTYUIOP"));
    TEST_ASSERT_EQUAL(AUTH_PASSWORD_COMMON, issue_of("1Q2W3E4R5T"));
    TEST_ASSERT_EQUAL(AUTH_PASSWORD_OK, issue_of("password1235"));     // one digit off the list
}

// --- Priority: too short -> too long -> admin -> all same -> sequential -> common ------------

void test_priority_order(void) {
    char buf[300];
    auth_logic_t ctx = make_ctx(10);

    // short beats every weak rule
    TEST_ASSERT_EQUAL(AUTH_PASSWORD_TOO_SHORT, issue_of("aaaa"));            // all same, but short
    TEST_ASSERT_EQUAL(AUTH_PASSWORD_TOO_SHORT, issue_of("abcdefgh"));        // sequential, but short

    // too long beats all-same and the weak rules
    memset(buf, 'a', sizeof(buf));
    TEST_ASSERT_EQUAL(AUTH_PASSWORD_TOO_LONG, auth_logic_password_issue(&ctx, buf, 129));
    memcpy(buf, "admin", 5);
    TEST_ASSERT_EQUAL(AUTH_PASSWORD_TOO_LONG, auth_logic_password_issue(&ctx, buf, 150));

    // admin beats common (the only overlap: "adminadmin" etc. are also in the common list)
    TEST_ASSERT_EQUAL(AUTH_PASSWORD_CONTAINS_ADMIN, issue_of("adminadmin"));
    TEST_ASSERT_EQUAL(AUTH_PASSWORD_CONTAINS_ADMIN, issue_of("admin12345"));

    // all same beats common ("1111111111" is also in the common list)
    TEST_ASSERT_EQUAL(AUTH_PASSWORD_ALL_SAME, issue_of("1111111111"));

    // No password can be both a run and in the common list, or both a run and contain "admin" / be all
    // the same, so the order between those pairs cannot be observed; it is fixed by the documented order.
}

// --- Configuration and arguments ---------------------------------------------------------

void test_config_minimum_length_is_honoured(void) {
    auth_logic_t ctx = make_ctx(12);
    TEST_ASSERT_EQUAL(AUTH_PASSWORD_TOO_SHORT, auth_logic_password_issue(&ctx, "aZ3$kP9#mQw", 11));
    TEST_ASSERT_EQUAL(AUTH_PASSWORD_OK, auth_logic_password_issue(&ctx, "aZ3$kP9#mQwE", 12));
}

void test_minimum_below_policy_floor_uses_floor(void) {
    // A config minimum below AUTH_LOGIC_MIN_PASSWORD_LEN cannot lower the floor of 10.
    auth_logic_t ctx = make_ctx(0);
    TEST_ASSERT_EQUAL(AUTH_PASSWORD_TOO_SHORT, auth_logic_password_issue(&ctx, "aZ3$kP9#m", 9));
    TEST_ASSERT_EQUAL(AUTH_PASSWORD_OK, auth_logic_password_issue(&ctx, "aZ3$kP9#mQ", 10));
}

void test_null_arguments(void) {
    TEST_ASSERT_EQUAL(AUTH_PASSWORD_TOO_SHORT, auth_logic_password_issue(NULL, NULL, 0));
    auth_logic_t ctx = make_ctx(10);
    TEST_ASSERT_EQUAL(AUTH_PASSWORD_TOO_SHORT, auth_logic_password_issue(&ctx, NULL, 20));
    // A NULL ctx uses the default minimum.
    TEST_ASSERT_EQUAL(AUTH_PASSWORD_OK, auth_logic_password_issue(NULL, "aZ3$kP9#mQ", 10));
    TEST_ASSERT_EQUAL(AUTH_PASSWORD_TOO_SHORT, auth_logic_password_issue(NULL, "aZ3$kP9#m", 9));
}

void test_does_not_modify_ctx(void) {
    auth_logic_t ctx = make_ctx(10);
    auth_logic_t before = ctx;
    (void)auth_logic_password_issue(&ctx, "1234567890", 10);
    TEST_ASSERT_EQUAL_MEMORY(&before, &ctx, sizeof(ctx));
}

// --- Agreement with the policy that existed before (fixed-seed random samples) --------------

static uint32_t g_rng = 0x12345678u;
static uint32_t rng_next(void) {   // xorshift32, deterministic
    uint32_t x = g_rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    g_rng = x;
    return x;
}
static uint32_t rng_range(uint32_t lo, uint32_t hi) { return lo + rng_next() % (hi - lo + 1); }

static size_t gen_sample(char *buf, uint32_t mode) {
    size_t len = 0;
    switch (mode) {
    case 0: // printable ASCII, any length up to 140
        len = rng_range(0, 140);
        for (size_t i = 0; i < len; i++) buf[i] = (char)rng_range(0x20, 0x7e);
        break;
    case 1: // lower-case letters only
        len = rng_range(8, 130);
        for (size_t i = 0; i < len; i++) buf[i] = (char)rng_range('a', 'z');
        break;
    case 2: { // an ascending or descending run
        len = rng_range(8, 40);
        char start = (char)rng_range(0x30, 0x7a);
        int step = (rng_next() & 1) ? 1 : -1;
        for (size_t i = 0; i < len; i++) buf[i] = (char)(start + step * (int)i);
        break;
    }
    case 3: { // one character repeated
        len = rng_range(5, 140);
        char c = (char)rng_range(0x21, 0x7e);
        memset(buf, c, len);
        break;
    }
    case 4: { // random text with "admin" in random case somewhere
        len = rng_range(10, 60);
        for (size_t i = 0; i < len; i++) buf[i] = (char)rng_range(0x21, 0x7e);
        size_t at = rng_range(0, (uint32_t)(len - 5));
        static const char *a = "admin";
        for (size_t i = 0; i < 5; i++) buf[at + i] = (rng_next() & 1) ? (char)(a[i] - 32) : a[i];
        break;
    }
    case 5: { // an entry of the common list with random case
        const char *e = k_common[rng_range(0, (uint32_t)K_COMMON_COUNT - 1)];
        len = strlen(e);
        for (size_t i = 0; i < len; i++) {
            char c = e[i];
            if (c >= 'a' && c <= 'z' && (rng_next() & 1)) c = (char)(c - 32);
            buf[i] = c;
        }
        break;
    }
    case 6: { // a run with one character changed
        len = rng_range(10, 30);
        char start = (char)rng_range(0x30, 0x70);
        for (size_t i = 0; i < len; i++) buf[i] = (char)(start + (int)i);
        buf[rng_range(0, (uint32_t)len - 1)] = (char)rng_range(0x21, 0x7e);
        break;
    }
    case 7: { // boundary lengths of an otherwise random alphanumeric text
        static const size_t lens[] = {8, 9, 10, 11, 127, 128, 129, 130};
        len = lens[rng_range(0, 7)];
        for (size_t i = 0; i < len; i++) {
            uint32_t r = rng_range(0, 61);
            buf[i] = (char)(r < 10 ? '0' + r : (r < 36 ? 'a' + (r - 10) : 'A' + (r - 36)));
        }
        break;
    }
    case 8: // bytes with the high bit set (UTF-8 like)
        len = rng_range(10, 128);
        for (size_t i = 0; i < len; i++) buf[i] = (char)rng_range(0x80, 0xff);
        break;
    default: // arbitrary bytes, including NUL
        len = rng_range(1, 140);
        for (size_t i = 0; i < len; i++) buf[i] = (char)rng_range(0, 0xff);
        break;
    }
    return len;
}

#define SAMPLE_COUNT 120000

void test_agrees_with_previous_policy_on_random_samples(void) {
    static const uint32_t mins[] = {10, 10, 10, 12, 16, 40};
    char buf[320];
    size_t ok_count = 0, weak_count = 0, short_count = 0, long_count = 0;

    g_rng = 0x12345678u;
    for (int n = 0; n < SAMPLE_COUNT; n++) {
        uint32_t mode = rng_range(0, 9);
        size_t len = gen_sample(buf, mode);
        uint32_t min_len = mins[rng_range(0, 5)];
        auth_logic_t ctx = make_ctx(min_len);

        // What auth_logic_setup_password() did before: length gate, then the weakness check.
        bool ref_len_ok = (len >= min_len && len <= AUTH_LOGIC_MAX_PASSWORD_LEN);
        bool ref_ok = ref_len_ok && !ref_is_weak_password(buf, len);

        auth_password_issue_e got = auth_logic_password_issue(&ctx, buf, len);

        char msg[96];
        snprintf(msg, sizeof(msg), "sample %d mode %u len %zu min %u issue %d", n, (unsigned)mode, len, (unsigned)min_len, (int)got);
        TEST_ASSERT_EQUAL_MESSAGE(ref_ok, got == AUTH_PASSWORD_OK, msg);

        if (ref_ok) ok_count++;
        else if (len < min_len || len < AUTH_LOGIC_MIN_PASSWORD_LEN) short_count++;
        else if (len > AUTH_LOGIC_MAX_PASSWORD_LEN) long_count++;
        else weak_count++;

        // The reason must also be consistent with the length rules.
        if (len < min_len || len < AUTH_LOGIC_MIN_PASSWORD_LEN) {
            TEST_ASSERT_EQUAL_MESSAGE(AUTH_PASSWORD_TOO_SHORT, got, msg);
        } else if (len > AUTH_LOGIC_MAX_PASSWORD_LEN) {
            TEST_ASSERT_EQUAL_MESSAGE(AUTH_PASSWORD_TOO_LONG, got, msg);
        }
    }
    // The generator must actually reach every outcome, or the comparison proves little.
    TEST_ASSERT_GREATER_THAN_size_t(5000, ok_count);
    TEST_ASSERT_GREATER_THAN_size_t(5000, weak_count);
    TEST_ASSERT_GREATER_THAN_size_t(1000, short_count);
    TEST_ASSERT_GREATER_THAN_size_t(1000, long_count);
}

void test_every_weak_reason_is_reached_by_random_samples(void) {
    char buf[320];
    size_t seen[8] = {0};
    g_rng = 0x2468ace0u;
    for (int n = 0; n < 60000; n++) {
        size_t len = gen_sample(buf, rng_range(0, 9));
        auth_logic_t ctx = make_ctx(10);
        auth_password_issue_e r = auth_logic_password_issue(&ctx, buf, len);
        TEST_ASSERT_TRUE(r >= AUTH_PASSWORD_OK && r <= AUTH_PASSWORD_COMMON);
        seen[r]++;
    }
    for (int r = AUTH_PASSWORD_OK; r <= AUTH_PASSWORD_COMMON; r++) {
        char msg[48];
        snprintf(msg, sizeof(msg), "reason %d never produced", r);
        TEST_ASSERT_GREATER_THAN_size_t_MESSAGE(0, seen[r], msg);
    }
}

void test_agrees_with_setup_password(void) {
    // auth_logic_setup_password() accepts exactly the passwords for which the issue is OK.
    // Rejections are cheap, so all of them are checked; only a few accepted ones are (PBKDF2 is slow).
    char buf[320];
    int accepted_checked = 0;
    g_rng = 0x0badf00du;
    for (int n = 0; n < 4000; n++) {
        size_t len = gen_sample(buf, rng_range(0, 9));
        auth_logic_config_t cfg;
        auth_logic_config_default(&cfg);
        auth_logic_t ctx;
        TEST_ASSERT_EQUAL(0, auth_logic_init(&ctx, &ref_crypto_if, &cfg));

        auth_password_issue_e issue = auth_logic_password_issue(&ctx, buf, len);
        if (issue == AUTH_PASSWORD_OK) {
            if (accepted_checked >= 15) continue;
            accepted_checked++;
            TEST_ASSERT_EQUAL(0, auth_logic_setup_password(&ctx, buf, len));
            TEST_ASSERT_EQUAL(AUTH_STATE_READY, ctx.state);
        } else {
            char msg[64];
            snprintf(msg, sizeof(msg), "sample %d len %zu issue %d", n, len, (int)issue);
            TEST_ASSERT_EQUAL_MESSAGE(-1, auth_logic_setup_password(&ctx, buf, len), msg);
            TEST_ASSERT_EQUAL(AUTH_STATE_NEEDS_SETUP, ctx.state);
        }
    }
    TEST_ASSERT_EQUAL(15, accepted_checked);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_acceptable_passwords);
    RUN_TEST(test_too_short);
    RUN_TEST(test_too_long);
    RUN_TEST(test_contains_admin);
    RUN_TEST(test_all_same);
    RUN_TEST(test_sequential);
    RUN_TEST(test_one_to_zero_is_not_a_run);
    RUN_TEST(test_common_every_list_entry);
    RUN_TEST(test_common_is_case_insensitive);
    RUN_TEST(test_priority_order);
    RUN_TEST(test_config_minimum_length_is_honoured);
    RUN_TEST(test_minimum_below_policy_floor_uses_floor);
    RUN_TEST(test_null_arguments);
    RUN_TEST(test_does_not_modify_ctx);
    RUN_TEST(test_agrees_with_previous_policy_on_random_samples);
    RUN_TEST(test_every_weak_reason_is_reached_by_random_samples);
    RUN_TEST(test_agrees_with_setup_password);
    return UNITY_END();
}
