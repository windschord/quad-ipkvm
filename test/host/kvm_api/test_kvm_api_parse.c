#include "unity.h"
#include "kvm_api.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* ボディの解析 {"channel":N} の表のテスト（docs/WEB_API.md 7.2 / 7.3）。
 * 入力は毎回、ちょうどの大きさのヒープ領域に複製して渡す（NUL 終端なし）。長さを超えて読めば ASan が捕まえる。 */

void setUp(void) {}
void tearDown(void) {}

#define SENTINEL 0xEE

/* body[0..n) を解析する。成功なら *out にチャンネル。失敗のとき *out が SENTINEL のままであることも確かめる */
static int parse_n(const char *s, size_t n, uint8_t *out) {
    char *copy = malloc(n ? n : 1);
    TEST_ASSERT_NOT_NULL(copy);
    if (n) memcpy(copy, s, n);
    *out = SENTINEL;
    int r = kvm_api_parse_channel(copy, n, out);
    free(copy);
    if (r != 0) TEST_ASSERT_EQUAL_UINT8(SENTINEL, *out);
    return r;
}

static void expect_ok(const char *s, uint8_t ch) {
    uint8_t out;
    char msg[160];
    snprintf(msg, sizeof(msg), "should be accepted: %s", s);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, parse_n(s, strlen(s), &out), msg);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(ch, out, msg);
}

static void expect_bad(const char *s) {
    uint8_t out;
    char msg[160];
    snprintf(msg, sizeof(msg), "should be rejected: %s", s);
    TEST_ASSERT_EQUAL_INT_MESSAGE(-1, parse_n(s, strlen(s), &out), msg);
}

void test_valid_channels(void) {
    expect_ok("{\"channel\":1}", 1);
    expect_ok("{\"channel\":2}", 2);
    expect_ok("{\"channel\":3}", 3);
    expect_ok("{\"channel\":4}", 4);
}

void test_valid_with_json_whitespace(void) {
    expect_ok("{ \"channel\" : 3 }", 3);
    expect_ok(" {\"channel\":2}", 2);
    expect_ok("{\"channel\":2} ", 2);
    expect_ok("\n{\t\"channel\"\r:\n4\t}\r\n", 4);
    expect_ok("{ \"channel\":1}", 1);
    expect_ok("{\"channel\" :1}", 1);
    expect_ok("{\"channel\": 1}", 1);
    expect_ok("{\"channel\":1 }", 1);
}

void test_range_boundaries(void) {
    expect_bad("{\"channel\":0}");
    expect_bad("{\"channel\":5}");
    expect_bad("{\"channel\":6}");
    expect_bad("{\"channel\":9}");
    expect_bad("{\"channel\":10}");
    expect_bad("{\"channel\":11}");
    expect_bad("{\"channel\":14}");
}

void test_missing_or_other_keys(void) {
    expect_bad("{}");
    expect_bad("{\"x\":1}");
    expect_bad("{\"Channel\":3}");
    expect_bad("{\"CHANNEL\":3}");
    expect_bad("{\"channel \":3}");
    expect_bad("{\" channel\":3}");
    expect_bad("{\"chan\":3}");
    expect_bad("{\"channels\":3}");
    expect_bad("{\"channe\":3}");
    expect_bad("{\"channel\":3,\"x\":1}");
    expect_bad("{\"x\":1,\"channel\":3}");
    expect_bad("{\"channel\":3,\"action\":\"press\"}");
    expect_bad("{\"channel\":3,\"duration_ms\":99999}");
    expect_bad("{\"duration_ms\":99999,\"channel\":3}");
}

void test_duplicate_keys(void) {
    expect_bad("{\"channel\":3,\"channel\":3}");
    expect_bad("{\"channel\":1,\"channel\":2}");
    expect_bad("{\"channel\":1,\"channel\":1,\"channel\":1}");
}

void test_escaped_key_is_rejected(void) {
    expect_bad("{\"chann\\u0065l\":3}");
    expect_bad("{\"\\u0063hannel\":3}");
    expect_bad("{\"chan\\nel\":3}");
}

void test_value_types(void) {
    expect_bad("{\"channel\":\"3\"}");
    expect_bad("{\"channel\":\"\"}");
    expect_bad("{\"channel\":null}");
    expect_bad("{\"channel\":true}");
    expect_bad("{\"channel\":false}");
    expect_bad("{\"channel\":[3]}");
    expect_bad("{\"channel\":{\"a\":3}}");
    expect_bad("{\"channel\":}");
    expect_bad("{\"channel\":,}");
    expect_bad("{\"channel\"}");
    expect_bad("{\"channel\":NaN}");
    expect_bad("{\"channel\":Infinity}");
}

void test_number_forms(void) {
    expect_bad("{\"channel\":3.0}");
    expect_bad("{\"channel\":3.}");
    expect_bad("{\"channel\":.3}");
    expect_bad("{\"channel\":3.5}");
    expect_bad("{\"channel\":3e0}");
    expect_bad("{\"channel\":3E0}");
    expect_bad("{\"channel\":3e+0}");
    expect_bad("{\"channel\":3e-0}");
    expect_bad("{\"channel\":0.3e1}");
    expect_bad("{\"channel\":-3}");
    expect_bad("{\"channel\":-0}");
    expect_bad("{\"channel\":+3}");
    expect_bad("{\"channel\":03}");
    expect_bad("{\"channel\":01}");
    expect_bad("{\"channel\":00}");
    expect_bad("{\"channel\":003}");
    expect_bad("{\"channel\":0x3}");
    expect_bad("{\"channel\":0b11}");
    expect_bad("{\"channel\":3_0}");
    expect_bad("{\"channel\":3 4}");
    expect_bad("{\"channel\":1 2}");
}

void test_huge_numbers_do_not_wrap_into_range(void) {
    expect_bad("{\"channel\":99999999999999999999999}");
    expect_bad("{\"channel\":4294967297}");    /* 2^32 + 1: 32 ビットで巻くと 1 */
    expect_bad("{\"channel\":4294967299}");    /* 2^32 + 3 */
    expect_bad("{\"channel\":18446744073709551617}"); /* 2^64 + 1 */
    expect_bad("{\"channel\":257}");           /* 8 ビットで巻くと 1 */
    expect_bad("{\"channel\":258}");
    expect_bad("{\"channel\":65537}");         /* 16 ビットで巻くと 1 */
    expect_bad("{\"channel\":65539}");
    expect_bad("{\"channel\":256}");
    expect_bad("{\"channel\":-4294967295}");
}

void test_structure_errors(void) {
    expect_bad("");
    expect_bad(" ");
    expect_bad("{");
    expect_bad("}");
    expect_bad("{\"channel\":3");
    expect_bad("\"channel\":3}");
    expect_bad("{\"channel\":3}}");
    expect_bad("{{\"channel\":3}}");
    expect_bad("{\"channel\":3},");
    expect_bad("{\"channel\":3,}");
    expect_bad("{,\"channel\":3}");
    expect_bad("{\"channel\":3}{}");
    expect_bad("{\"channel\":3}{\"channel\":3}");
    expect_bad("{\"channel\":3};");
    expect_bad("{\"channel\" 3}");
    expect_bad("{\"channel\"::3}");
    expect_bad("{\"channel\";3}");
    expect_bad("{channel:3}");
    expect_bad("{'channel':3}");
    expect_bad("{\"channel\":'3'}");
    expect_bad("[{\"channel\":3}]");
    expect_bad("[3]");
    expect_bad("3");
    expect_bad("\"channel\"");
    expect_bad("null");
    expect_bad("true");
}

void test_extra_characters_after_object(void) {
    expect_bad("{\"channel\":3}x");
    expect_bad("{\"channel\":3} x");
    expect_bad("{\"channel\":3}\n\n0");
    expect_bad("{\"channel\":3}/* c */");
    expect_bad("{\"channel\":3}//");
    expect_bad("x{\"channel\":3}");
    expect_bad("/**/{\"channel\":3}");
}

void test_non_json_whitespace_is_rejected(void) {
    expect_bad("\v{\"channel\":3}");
    expect_bad("\f{\"channel\":3}");
    expect_bad("{\"channel\":3}\v");
    expect_bad("{\"channel\":\f3}");
    expect_bad("\xC2\xA0{\"channel\":3}");           /* NBSP */
    expect_bad("\xEF\xBB\xBF{\"channel\":3}");        /* BOM */
    expect_bad("{\"channel\":\xEF\xBC\x93}");         /* 全角の 3 */
    expect_bad("{\"channel\":3\xE2\x80\x8B}");        /* ゼロ幅スペース */
}

void test_nul_bytes_are_rejected(void) {
    uint8_t out;
    static const char a[] = "{\"channel\":3}\0";
    TEST_ASSERT_EQUAL_INT(-1, parse_n(a, sizeof(a) - 1, &out)); /* 末尾の NUL を含む 14 バイト */
    static const char b[] = "{\"channel\":\0 3}";
    TEST_ASSERT_EQUAL_INT(-1, parse_n(b, sizeof(b) - 1, &out));
    static const char c[] = "{\"chan\0nel\":3}";
    TEST_ASSERT_EQUAL_INT(-1, parse_n(c, sizeof(c) - 1, &out));
    static const char d[] = "\0{\"channel\":3}";
    TEST_ASSERT_EQUAL_INT(-1, parse_n(d, sizeof(d) - 1, &out));
    static const char e[] = "{\"channel\":3}\0\0\0";
    TEST_ASSERT_EQUAL_INT(-1, parse_n(e, sizeof(e) - 1, &out));
    static const char f[] = "\0";
    TEST_ASSERT_EQUAL_INT(-1, parse_n(f, 1, &out));
    /* NUL で切れた文字列としては正しくても、len が NUL の後まで含むなら不正 */
    static const char g[] = "{\"channel\":3}\0garbage";
    TEST_ASSERT_EQUAL_INT(-1, parse_n(g, sizeof(g) - 1, &out));
    /* len が NUL の手前までなら正しい */
    TEST_ASSERT_EQUAL_INT(0, parse_n(g, 13, &out));
    TEST_ASSERT_EQUAL_UINT8(3, out);
}

/* "{"channel":3}" の後ろ（または前）を空白で埋めて、ちょうど total バイトにする */
static size_t padded(char *buf, size_t total, bool pad_front) {
    static const char core[] = "{\"channel\":3}";
    size_t n = sizeof(core) - 1;
    size_t pad = total - n;
    if (pad_front) {
        memset(buf, ' ', pad);
        memcpy(buf + pad, core, n);
    } else {
        memcpy(buf, core, n);
        memset(buf + n, ' ', pad);
    }
    return total;
}

void test_length_limit_64(void) {
    char buf[2048];
    uint8_t out;
    /* 13 バイト(最短)、63、64: 受理 */
    TEST_ASSERT_EQUAL_INT(0, parse_n("{\"channel\":3}", 13, &out));
    TEST_ASSERT_EQUAL_INT(0, parse_n(buf, padded(buf, 63, false), &out));
    TEST_ASSERT_EQUAL_UINT8(3, out);
    TEST_ASSERT_EQUAL_INT(0, parse_n(buf, padded(buf, 64, false), &out));
    TEST_ASSERT_EQUAL_UINT8(3, out);
    TEST_ASSERT_EQUAL_INT(0, parse_n(buf, padded(buf, 64, true), &out));
    TEST_ASSERT_EQUAL_UINT8(3, out);
    /* 65 バイト以上: 内容が正しくても不正 */
    TEST_ASSERT_EQUAL_INT(-1, parse_n(buf, padded(buf, 65, false), &out));
    TEST_ASSERT_EQUAL_INT(-1, parse_n(buf, padded(buf, 65, true), &out));
    TEST_ASSERT_EQUAL_INT(-1, parse_n(buf, padded(buf, 66, false), &out));
    TEST_ASSERT_EQUAL_INT(-1, parse_n(buf, padded(buf, 128, false), &out));
    TEST_ASSERT_EQUAL_INT(-1, parse_n(buf, padded(buf, 1000, false), &out));
    TEST_ASSERT_EQUAL_INT(-1, parse_n(buf, padded(buf, 2048, true), &out));
    /* 定数そのもの */
    TEST_ASSERT_EQUAL_UINT(64, KVM_API_BODY_MAX);
}

void test_null_arguments(void) {
    uint8_t out = SENTINEL;
    TEST_ASSERT_EQUAL_INT(-1, kvm_api_parse_channel(NULL, 0, &out));
    TEST_ASSERT_EQUAL_INT(-1, kvm_api_parse_channel(NULL, 13, &out));
    TEST_ASSERT_EQUAL_INT(-1, kvm_api_parse_channel("{\"channel\":3}", 13, NULL));
    TEST_ASSERT_EQUAL_INT(-1, kvm_api_parse_channel("{\"channel\":3}", 0, &out));
    TEST_ASSERT_EQUAL_UINT8(SENTINEL, out);
}

/* 先頭からの全部の接頭辞は、完全な本文でない限り不正（途中で切れた入力で読み過ぎない） */
void test_every_proper_prefix_is_rejected(void) {
    static const char full[] = "{ \"channel\" : 3 }";
    size_t n = sizeof(full) - 1;
    uint8_t out;
    for (size_t k = 0; k < n; k++) {
        TEST_ASSERT_EQUAL_INT(-1, parse_n(full, k, &out));
    }
    TEST_ASSERT_EQUAL_INT(0, parse_n(full, n, &out));
    TEST_ASSERT_EQUAL_UINT8(3, out);
}

/* 1 バイトだけ別の値にした本文は、すべて不正か、同じチャンネルのまま（空白への置換で受理されることはない） */
void test_single_byte_substitutions(void) {
    static const char full[] = "{\"channel\":3}";
    size_t n = sizeof(full) - 1;
    for (size_t pos = 0; pos < n; pos++) {
        for (int v = 0; v < 256; v++) {
            char tmp[16];
            memcpy(tmp, full, n);
            tmp[pos] = (char)v;
            uint8_t out;
            int r = parse_n(tmp, n, &out);
            if (r == 0) {
                /* 受理されるのは: 同じ本文、または '3' を '1'..'4' に変えたとき、または空白の位置の変更（ここでは無い） */
                bool is_value_pos = (pos == 11);
                TEST_ASSERT_TRUE_MESSAGE(memcmp(tmp, full, n) == 0 || (is_value_pos && v >= '1' && v <= '4'),
                                         "unexpected accept");
                TEST_ASSERT_EQUAL_UINT8(is_value_pos ? (uint8_t)(v - '0') : 3, out);
            }
        }
    }
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_valid_channels);
    RUN_TEST(test_valid_with_json_whitespace);
    RUN_TEST(test_range_boundaries);
    RUN_TEST(test_missing_or_other_keys);
    RUN_TEST(test_duplicate_keys);
    RUN_TEST(test_escaped_key_is_rejected);
    RUN_TEST(test_value_types);
    RUN_TEST(test_number_forms);
    RUN_TEST(test_huge_numbers_do_not_wrap_into_range);
    RUN_TEST(test_structure_errors);
    RUN_TEST(test_extra_characters_after_object);
    RUN_TEST(test_non_json_whitespace_is_rejected);
    RUN_TEST(test_nul_bytes_are_rejected);
    RUN_TEST(test_length_limit_64);
    RUN_TEST(test_null_arguments);
    RUN_TEST(test_every_proper_prefix_is_rejected);
    RUN_TEST(test_single_byte_substitutions);
    return UNITY_END();
}
