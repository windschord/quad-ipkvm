#include "unity.h"
#include "http_guard.h"
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

void test_json_setup_valid(void) {
    http_guard_json_setup_t st;
    const char *json = "{\"password\":\"my_secret123\"}";
    TEST_ASSERT_EQUAL_INT(0, http_guard_parse_json_password(json, strlen(json), &st));
    TEST_ASSERT_EQUAL_STRING("my_secret123", st.password);
    TEST_ASSERT_EQUAL_size_t(12, st.password_len);
}

void test_json_setup_with_spaces(void) {
    http_guard_json_setup_t st;
    const char *json = " { \n\t \"password\"  : \r \"my_secret123\" \n } ";
    TEST_ASSERT_EQUAL_INT(0, http_guard_parse_json_password(json, strlen(json), &st));
    TEST_ASSERT_EQUAL_STRING("my_secret123", st.password);
}

void test_json_setup_escapes(void) {
    http_guard_json_setup_t st;
    const char *json = "{\"password\":\"a\\\"b\\\\c\\/d\\be\\ff\\ng\\rh\\ti\\u3042j\"}";
    TEST_ASSERT_EQUAL_INT(0, http_guard_parse_json_password(json, strlen(json), &st));
    TEST_ASSERT_EQUAL_STRING("a\"b\\c/d\be\ff\ng\rh\ti\xE3\x81\x82j", st.password);
}

void test_json_setup_surrogates(void) {
    http_guard_json_setup_t st;
    const char *json = "{\"password\":\"\\uD83D\\uDE00\"}"; // Smiley face
    TEST_ASSERT_EQUAL_INT(0, http_guard_parse_json_password(json, strlen(json), &st));
    TEST_ASSERT_EQUAL_STRING("\xF0\x9F\x98\x80", st.password);
    TEST_ASSERT_EQUAL_size_t(4, st.password_len);
}

void test_json_setup_reject_isolated_surrogates(void) {
    http_guard_json_setup_t st;
    const char *json_high = "{\"password\":\"\\uD83D\"}";
    const char *json_low = "{\"password\":\"\\uDE00\"}";
    const char *json_bad_pair = "{\"password\":\"\\uD83D\\u1234\"}";
    TEST_ASSERT_NOT_EQUAL_INT(0, http_guard_parse_json_password(json_high, strlen(json_high), &st));
    TEST_ASSERT_NOT_EQUAL_INT(0, http_guard_parse_json_password(json_low, strlen(json_low), &st));
    TEST_ASSERT_NOT_EQUAL_INT(0, http_guard_parse_json_password(json_bad_pair, strlen(json_bad_pair), &st));
}

void test_json_setup_reject_invalid_utf8(void) {
    http_guard_json_setup_t st;
    // 0xFF 0xFE is not valid UTF-8
    const char *json1 = "{\"password\":\"\xFF\xFE\"}";
    TEST_ASSERT_NOT_EQUAL_INT(0, http_guard_parse_json_password(json1, strlen(json1), &st));

    // Overlong encoding for 'A'
    const char *json2 = "{\"password\":\"\xC1\x81\"}";
    TEST_ASSERT_NOT_EQUAL_INT(0, http_guard_parse_json_password(json2, strlen(json2), &st));
}

void test_json_setup_reject_null_byte(void) {
    http_guard_json_setup_t st;
    const char *json = "{\"password\":\"foo\\u0000bar\"}";
    TEST_ASSERT_NOT_EQUAL_INT(0, http_guard_parse_json_password(json, strlen(json), &st));
}

void test_json_setup_reject_unknown_key(void) {
    http_guard_json_setup_t st;
    const char *json = "{\"password\":\"sec\",\"foo\":\"bar\"}";
    TEST_ASSERT_NOT_EQUAL_INT(0, http_guard_parse_json_password(json, strlen(json), &st));
}

void test_json_setup_reject_duplicate_key(void) {
    http_guard_json_setup_t st;
    const char *json = "{\"password\":\"sec\",\"password\":\"sec2\"}";
    TEST_ASSERT_NOT_EQUAL_INT(0, http_guard_parse_json_password(json, strlen(json), &st));
}

void test_json_setup_length_boundary(void) {
    http_guard_json_setup_t st;
    char json[200];

    // Exactly 128 bytes (should be allowed)
    strcpy(json, "{\"password\":\"");
    for(int i=0; i<128; i++) {
        strcat(json, "A");
    }
    strcat(json, "\"}");
    TEST_ASSERT_EQUAL_INT(0, http_guard_parse_json_password(json, strlen(json), &st));
    TEST_ASSERT_EQUAL_size_t(128, st.password_len);

    // 129 bytes (should be rejected)
    strcpy(json, "{\"password\":\"");
    for(int i=0; i<129; i++) {
        strcat(json, "A");
    }
    strcat(json, "\"}");
    TEST_ASSERT_NOT_EQUAL_INT(0, http_guard_parse_json_password(json, strlen(json), &st));
}

void test_json_change_valid(void) {
    http_guard_json_change_t st;
    const char *json = "{\"old_password\":\"old123\",\"new_password\":\"new456\"}";
    TEST_ASSERT_EQUAL_INT(0, http_guard_parse_json_change(json, strlen(json), &st));
    TEST_ASSERT_EQUAL_STRING("old123", st.old_password);
    TEST_ASSERT_EQUAL_STRING("new456", st.new_password);
}

void test_json_change_missing_key(void) {
    http_guard_json_change_t st;
    const char *json = "{\"old_password\":\"old123\"}";
    TEST_ASSERT_NOT_EQUAL_INT(0, http_guard_parse_json_change(json, strlen(json), &st));
}

void test_json_reject_trailing_garbage(void) {
    http_guard_json_setup_t st;
    const char *json = "{\"password\":\"my_secret123\"} garbage";
    TEST_ASSERT_NOT_EQUAL_INT(0, http_guard_parse_json_password(json, strlen(json), &st));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_json_setup_valid);
    RUN_TEST(test_json_setup_with_spaces);
    RUN_TEST(test_json_setup_escapes);
    RUN_TEST(test_json_setup_surrogates);
    RUN_TEST(test_json_setup_reject_isolated_surrogates);
    RUN_TEST(test_json_setup_reject_invalid_utf8);
    RUN_TEST(test_json_setup_reject_null_byte);
    RUN_TEST(test_json_setup_reject_unknown_key);
    RUN_TEST(test_json_setup_reject_duplicate_key);
    RUN_TEST(test_json_setup_length_boundary);
    RUN_TEST(test_json_change_valid);
    RUN_TEST(test_json_change_missing_key);
    RUN_TEST(test_json_reject_trailing_garbage);
    return UNITY_END();
}
