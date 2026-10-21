#include "unity.h"
#include "http_guard.h"
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

void test_cookie_valid_single(void) {
    uint8_t token[32];
    const char *header = "session=AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";
    TEST_ASSERT_EQUAL_INT(0, http_guard_extract_session_cookie(header, strlen(header), token));

    uint8_t expected[32] = {0};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, token, 32);
}

void test_cookie_valid_with_others(void) {
    uint8_t token[32];
    const char *header = "foo=bar; session=AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA; baz=qux";
    TEST_ASSERT_EQUAL_INT(0, http_guard_extract_session_cookie(header, strlen(header), token));
}

void test_cookie_reject_duplicate(void) {
    uint8_t token[32];
    const char *header = "session=AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA; session=BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB";
    TEST_ASSERT_NOT_EQUAL_INT(0, http_guard_extract_session_cookie(header, strlen(header), token));
}

void test_cookie_reject_wrong_size(void) {
    uint8_t token[32];
    const char *header = "session=AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";
    TEST_ASSERT_NOT_EQUAL_INT(0, http_guard_extract_session_cookie(header, strlen(header), token));
}

void test_cookie_no_session(void) {
    uint8_t token[32];
    const char *header = "foo=bar; baz=qux";
    TEST_ASSERT_NOT_EQUAL_INT(0, http_guard_extract_session_cookie(header, strlen(header), token));
}

void test_cookie_empty(void) {
    uint8_t token[32];
    TEST_ASSERT_NOT_EQUAL_INT(0, http_guard_extract_session_cookie("", 0, token));
}

void test_cookie_spaces(void) {
    uint8_t token[32];
    const char *header = "  session = AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA  ; ";
    TEST_ASSERT_EQUAL_INT(0, http_guard_extract_session_cookie(header, strlen(header), token));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_cookie_valid_single);
    RUN_TEST(test_cookie_valid_with_others);
    RUN_TEST(test_cookie_reject_duplicate);
    RUN_TEST(test_cookie_reject_wrong_size);
    RUN_TEST(test_cookie_no_session);
    RUN_TEST(test_cookie_empty);
    RUN_TEST(test_cookie_spaces);
    return UNITY_END();
}
