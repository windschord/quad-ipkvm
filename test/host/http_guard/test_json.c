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

void test_json_setup_reject_too_long(void) {
    http_guard_json_setup_t st;
    char json[200];
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
    RUN_TEST(test_json_setup_reject_unknown_key);
    RUN_TEST(test_json_setup_reject_duplicate_key);
    RUN_TEST(test_json_setup_reject_too_long);
    RUN_TEST(test_json_change_valid);
    RUN_TEST(test_json_change_missing_key);
    RUN_TEST(test_json_reject_trailing_garbage);
    return UNITY_END();
}
