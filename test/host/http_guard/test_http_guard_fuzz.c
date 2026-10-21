#include "unity.h"
#include "http_guard.h"
#include <stdint.h>
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static uint32_t xorshift_state = 1;

static uint32_t xorshift32(void) {
    uint32_t x = xorshift_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    xorshift_state = x;
    return x;
}

void test_fuzz_json(void) {
    xorshift_state = 0x12345678;
    char buf[512];
    http_guard_json_setup_t out;

    for (int i = 0; i < 10000; i++) {
        size_t len = xorshift32() % (sizeof(buf) - 1);
        for (size_t j = 0; j < len; j++) {
            buf[j] = (char)(xorshift32() & 0xFF);
        }
        buf[len] = '\0';
        http_guard_parse_json_password(buf, len, &out);
    }
}

void test_fuzz_cookie(void) {
    xorshift_state = 0x87654321;
    char buf[512];
    uint8_t token[32];

    for (int i = 0; i < 10000; i++) {
        size_t len = xorshift32() % (sizeof(buf) - 1);
        for (size_t j = 0; j < len; j++) {
            buf[j] = (char)(xorshift32() & 0xFF);
        }
        buf[len] = '\0';
        http_guard_extract_session_cookie(buf, len, token);
    }
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_fuzz_json);
    RUN_TEST(test_fuzz_cookie);
    return UNITY_END();
}
