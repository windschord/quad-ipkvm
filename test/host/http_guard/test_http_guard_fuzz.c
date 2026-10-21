#include "unity.h"
#include "http_guard.h"
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

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

// Generate a valid random UTF-8 string up to max_len bytes, returns the exact byte length
static size_t gen_random_utf8(char *buf, size_t max_len) {
    size_t len = 0;
    while (len < max_len) {
        uint32_t cp = xorshift32() % 0x110000;
        if (cp >= 0xD800 && cp <= 0xDFFF) continue; // Skip surrogates
        if (cp == 0) continue; // Skip null

        if (cp <= 0x7F) {
            if (len + 1 > max_len) break;
            buf[len++] = cp;
        } else if (cp <= 0x7FF) {
            if (len + 2 > max_len) break;
            buf[len++] = 0xC0 | (cp >> 6);
            buf[len++] = 0x80 | (cp & 0x3F);
        } else if (cp <= 0xFFFF) {
            if (len + 3 > max_len) break;
            buf[len++] = 0xE0 | (cp >> 12);
            buf[len++] = 0x80 | ((cp >> 6) & 0x3F);
            buf[len++] = 0x80 | (cp & 0x3F);
        } else {
            if (len + 4 > max_len) break;
            buf[len++] = 0xF0 | (cp >> 18);
            buf[len++] = 0x80 | ((cp >> 12) & 0x3F);
            buf[len++] = 0x80 | ((cp >> 6) & 0x3F);
            buf[len++] = 0x80 | (cp & 0x3F);
        }
    }
    buf[len] = '\0';
    return len;
}

// Escape UTF-8 to JSON string into out_buf (returns len)
static size_t escape_to_json(const char *in, size_t in_len, char *out, size_t max_out) {
    size_t len = 0;
    for (size_t i = 0; i < in_len; i++) {
        unsigned char c = in[i];
        if (c == '"' || c == '\\') {
            if (len + 2 > max_out) break;
            out[len++] = '\\';
            out[len++] = c;
        } else if (c < 0x20) {
            if (len + 6 > max_out) break;
            if (c == '\n') { out[len++] = '\\'; out[len++] = 'n'; }
            else if (c == '\r') { out[len++] = '\\'; out[len++] = 'r'; }
            else if (c == '\t') { out[len++] = '\\'; out[len++] = 't'; }
            else if (c == '\b') { out[len++] = '\\'; out[len++] = 'b'; }
            else if (c == '\f') { out[len++] = '\\'; out[len++] = 'f'; }
            else { len += snprintf(out + len, max_out - len, "\\u%04X", c); }
        } else {
            if (len + 1 > max_out) break;
            out[len++] = c;
        }
    }
    out[len] = '\0';
    return len;
}

void test_fuzz_json_property(void) {
    char original[AUTH_LOGIC_MAX_PASSWORD_LEN + 1];
    char json_buf[1024];
    http_guard_json_setup_t out;

    uint32_t seeds[] = { 1, 42, 1234, 0x12345678, 0x87654321, 0xDEADBEEF, 0xCAFEBABE, 999, 1000, 2000, 3000, 4000, 5000, 6000, 7000, 8000 };

    for (int seed_idx = 0; seed_idx < 16; seed_idx++) {
        xorshift_state = seeds[seed_idx];

        for (int i = 0; i < 1000; i++) {
            size_t target_len = xorshift32() % (AUTH_LOGIC_MAX_PASSWORD_LEN + 1);
            size_t actual_len = gen_random_utf8(original, target_len);

            char escaped[512];
            escape_to_json(original, actual_len, escaped, sizeof(escaped)-1);

            int len = snprintf(json_buf, sizeof(json_buf), "{\"password\":\"%s\"}", escaped);

            int res = http_guard_parse_json_password(json_buf, len, &out);

            TEST_ASSERT_TRUE(res <= 0);

            if (res == 0) {
                TEST_ASSERT_EQUAL_size_t(actual_len, out.password_len);
                TEST_ASSERT_EQUAL_size_t(actual_len, strlen(out.password));
                TEST_ASSERT_EQUAL_STRING(original, out.password);
            }

            if (len > 0) {
                char mut_buf[1024];
                strcpy(mut_buf, json_buf);
                int mut_idx = xorshift32() % len;
                mut_buf[mut_idx] ^= 0x01;
                int m_res = http_guard_parse_json_password(mut_buf, len, &out);
                if (m_res == 0) {
                    TEST_ASSERT_TRUE(out.password_len <= AUTH_LOGIC_MAX_PASSWORD_LEN);
                    TEST_ASSERT_EQUAL_size_t(out.password_len, strlen(out.password));
                }
            }

            if (len > 5) {
                int m_res = http_guard_parse_json_password(json_buf, len - 3, &out);
                TEST_ASSERT_NOT_EQUAL_INT(0, m_res);
            }

            char dup_buf[1024];
            int dlen = snprintf(dup_buf, sizeof(dup_buf), "{\"password\":\"%s\", \"password\":\"dup\"}", escaped);
            TEST_ASSERT_NOT_EQUAL_INT(0, http_guard_parse_json_password(dup_buf, dlen, &out));
        }
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
        int res = http_guard_extract_session_cookie(buf, len, token);
        TEST_ASSERT_TRUE(res <= 0);
    }
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_fuzz_json_property);
    RUN_TEST(test_fuzz_cookie);
    return UNITY_END();
}
