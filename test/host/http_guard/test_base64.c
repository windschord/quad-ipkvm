#include "unity.h"
#include "http_guard.h"
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

void test_base64url_decode_rfc4648(void) {
    uint8_t buf[32];
    size_t out_len;

    TEST_ASSERT_EQUAL_INT(0, http_guard_base64url_decode("", 0, buf, sizeof(buf), &out_len));
    TEST_ASSERT_EQUAL_size_t(0, out_len);

    TEST_ASSERT_EQUAL_INT(0, http_guard_base64url_decode("Zg", 2, buf, sizeof(buf), &out_len));
    TEST_ASSERT_EQUAL_size_t(1, out_len);
    TEST_ASSERT_EQUAL_HEX8('f', buf[0]);

    TEST_ASSERT_EQUAL_INT(0, http_guard_base64url_decode("Zm8", 3, buf, sizeof(buf), &out_len));
    TEST_ASSERT_EQUAL_size_t(2, out_len);
    TEST_ASSERT_EQUAL_STRING_LEN("fo", buf, 2);

    TEST_ASSERT_EQUAL_INT(0, http_guard_base64url_decode("Zm9v", 4, buf, sizeof(buf), &out_len));
    TEST_ASSERT_EQUAL_size_t(3, out_len);
    TEST_ASSERT_EQUAL_STRING_LEN("foo", buf, 3);

    TEST_ASSERT_EQUAL_INT(0, http_guard_base64url_decode("Zm9vYg", 6, buf, sizeof(buf), &out_len));
    TEST_ASSERT_EQUAL_size_t(4, out_len);
    TEST_ASSERT_EQUAL_STRING_LEN("foob", buf, 4);

    TEST_ASSERT_EQUAL_INT(0, http_guard_base64url_decode("Zm9vYmE", 7, buf, sizeof(buf), &out_len));
    TEST_ASSERT_EQUAL_size_t(5, out_len);
    TEST_ASSERT_EQUAL_STRING_LEN("fooba", buf, 5);

    TEST_ASSERT_EQUAL_INT(0, http_guard_base64url_decode("Zm9vYmFy", 8, buf, sizeof(buf), &out_len));
    TEST_ASSERT_EQUAL_size_t(6, out_len);
    TEST_ASSERT_EQUAL_STRING_LEN("foobar", buf, 6);
}

void test_base64url_decode_reject_padding(void) {
    uint8_t buf[32];
    size_t out_len;
    TEST_ASSERT_NOT_EQUAL_INT(0, http_guard_base64url_decode("Zg==", 4, buf, sizeof(buf), &out_len));
}

void test_base64url_encode_rfc4648(void) {
    char buf[32];
    size_t out_len;

    TEST_ASSERT_EQUAL_INT(0, http_guard_base64url_encode((const uint8_t *)"", 0, buf, sizeof(buf), &out_len));
    TEST_ASSERT_EQUAL_size_t(0, out_len);
    TEST_ASSERT_EQUAL_STRING("", buf);

    TEST_ASSERT_EQUAL_INT(0, http_guard_base64url_encode((const uint8_t *)"f", 1, buf, sizeof(buf), &out_len));
    TEST_ASSERT_EQUAL_size_t(2, out_len);
    TEST_ASSERT_EQUAL_STRING("Zg", buf);

    TEST_ASSERT_EQUAL_INT(0, http_guard_base64url_encode((const uint8_t *)"fo", 2, buf, sizeof(buf), &out_len));
    TEST_ASSERT_EQUAL_size_t(3, out_len);
    TEST_ASSERT_EQUAL_STRING("Zm8", buf);

    TEST_ASSERT_EQUAL_INT(0, http_guard_base64url_encode((const uint8_t *)"foo", 3, buf, sizeof(buf), &out_len));
    TEST_ASSERT_EQUAL_size_t(4, out_len);
    TEST_ASSERT_EQUAL_STRING("Zm9v", buf);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_base64url_decode_rfc4648);
    RUN_TEST(test_base64url_decode_reject_padding);
    RUN_TEST(test_base64url_encode_rfc4648);
    return UNITY_END();
}
