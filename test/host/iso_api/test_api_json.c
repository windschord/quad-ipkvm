/*
 * iso_api の JSON のボディの解析(iso_api_parse_body)。ISO_MOUNT_DESIGN.md 7: `\u` エスケープは復号してから検査する(サロゲートペアは
 * 4 バイトの UTF-8、対にならないサロゲートは拒否)。名前の規則(C0 / DEL / C1、UTF-8 の検査、63 バイト)、size の範囲、sha256 の形式。
 * 期待値は手書き(UTF-8 のバイト列は、Unicode の符号位置から手で書いた)。
 */
#include "unity.h"
#include "iso_api.h"
#include "iso_api_priv.h"
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

#define BEGIN ISO_API_SCHEMA_BEGIN
#define MOUNT ISO_API_SCHEMA_MOUNT
#define EJECT ISO_API_SCHEMA_EJECT
#define RESCAN ISO_API_SCHEMA_RESCAN
#define FINISH ISO_API_SCHEMA_FINISH

static int parse(iso_api_schema_t s, const char *j, iso_api_body_t *b) {
    memset(b, 0xCC, sizeof(*b));
    return iso_api_parse_body(s, j, strlen(j), b);
}

/* begin: 正しい本体の名前を、バイト列で比べる */
static void expect_begin_name(const char *json, const char *name, size_t name_len) {
    iso_api_body_t b;
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, parse(BEGIN, json, &b), json);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(name_len, b.name_len, json);
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(name, b.name, name_len, json);
    TEST_ASSERT_EQUAL_UINT8(0, (uint8_t)b.name[name_len]); /* NUL 終端 */
}

static void expect_begin_bad(const char *json) {
    iso_api_body_t b;
    TEST_ASSERT_EQUAL_INT_MESSAGE(-1, parse(BEGIN, json, &b), json);
}


/* {"name":"n","size":34816,"sha256":"<0 を 64 個。pos 番目だけ ch>"} */
static const char *sha_json(char *out, size_t cap, int pos, char ch) {
    char sha[65];
    memset(sha, '0', 64);
    sha[64] = '\0';
    sha[pos] = ch;
    snprintf(out, cap, "{\"name\":\"n\",\"size\":34816,\"sha256\":\"%s\"}", sha);
    return out;
}

/* ---- begin: 正しい本体 -------------------------------------------------------------------- */

void test_begin_minimal_and_full(void) {
    iso_api_body_t b;
    TEST_ASSERT_EQUAL_INT(0, parse(BEGIN, "{\"name\":\"a\",\"size\":34816}", &b));
    TEST_ASSERT_EQUAL_STRING("a", b.name);
    TEST_ASSERT_EQUAL_UINT(1, b.name_len);
    TEST_ASSERT_EQUAL_UINT64(34816, b.size);
    TEST_ASSERT_FALSE(b.has_sha);

    const char *full =
        "{\"name\":\"ubuntu-24.04\",\"size\":6126805504,"
        "\"sha256\":\"00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff\"}";
    TEST_ASSERT_EQUAL_INT(0, parse(BEGIN, full, &b));
    TEST_ASSERT_EQUAL_STRING("ubuntu-24.04", b.name);
    TEST_ASSERT_EQUAL_UINT(12, b.name_len);
    TEST_ASSERT_EQUAL_UINT64(6126805504ull, b.size);
    TEST_ASSERT_TRUE(b.has_sha);
    for (int i = 0; i < 16; i++) {
        TEST_ASSERT_EQUAL_UINT8((uint8_t)(i * 0x11), b.sha[i]);
        TEST_ASSERT_EQUAL_UINT8((uint8_t)(i * 0x11), b.sha[16 + i]);
    }
}

void test_begin_whitespace_and_key_order(void) {
    iso_api_body_t b;
    TEST_ASSERT_EQUAL_INT(0, parse(BEGIN, " \t\r\n{ \n\"name\" : \"x\" ,\t\"size\"\r:\n34816 } \n", &b));
    TEST_ASSERT_EQUAL_STRING("x", b.name);
    TEST_ASSERT_EQUAL_UINT64(34816, b.size);
    TEST_ASSERT_EQUAL_INT(0, parse(BEGIN, "{\"size\":34816,\"name\":\"y\"}", &b));
    TEST_ASSERT_EQUAL_STRING("y", b.name);
    TEST_ASSERT_EQUAL_UINT64(34816, b.size);
    TEST_ASSERT_EQUAL_INT(0, parse(BEGIN, "{\"sha256\":\"00112233445566778899AABBCCDDEEFF00112233445566778899aabbccddeeff\",\"size\":34816,\"name\":\"z\"}", &b));
    TEST_ASSERT_TRUE(b.has_sha);
    TEST_ASSERT_EQUAL_UINT8(0x55, b.sha[5]);
    TEST_ASSERT_EQUAL_UINT8(0xAA, b.sha[10]);
    TEST_ASSERT_EQUAL_UINT8(0xFF, b.sha[15]);
    TEST_ASSERT_EQUAL_UINT8(0xAA, b.sha[26]);
}

void test_begin_sha256_accepts_either_case(void) {
    iso_api_body_t b;
    TEST_ASSERT_EQUAL_INT(0, parse(BEGIN, "{\"name\":\"n\",\"size\":34816,\"sha256\":\"ABCDEFabcdef0123456789ABCDEFabcdef0123456789ABCDEFabcdef01234567\"}", &b));
    static const uint8_t EXPECT[32] = {0xAB, 0xCD, 0xEF, 0xAB, 0xCD, 0xEF, 0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF, 0xAB, 0xCD,
                                       0xEF, 0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF, 0xAB, 0xCD, 0xEF, 0x01, 0x23, 0x45, 0x67};
    TEST_ASSERT_EQUAL_MEMORY(EXPECT, b.sha, 32);
    /* 16 進でない文字(各桁の範囲の、すぐ外側を含む)。0 を 64 個並べた文字列の pos 番目を置き換える */
    iso_api_body_t b2;
    char json[256];
    TEST_ASSERT_EQUAL_INT(0, parse(BEGIN, sha_json(json, sizeof(json), 0, '0'), &b2)); /* 基準: すべて 0 は正しい */
    TEST_ASSERT_EQUAL_INT(0, parse(BEGIN, sha_json(json, sizeof(json), 63, '9'), &b2));
    TEST_ASSERT_EQUAL_INT(0, parse(BEGIN, sha_json(json, sizeof(json), 63, 'a'), &b2));
    TEST_ASSERT_EQUAL_INT(0, parse(BEGIN, sha_json(json, sizeof(json), 63, 'f'), &b2));
    TEST_ASSERT_EQUAL_INT(0, parse(BEGIN, sha_json(json, sizeof(json), 63, 'A'), &b2));
    TEST_ASSERT_EQUAL_INT(0, parse(BEGIN, sha_json(json, sizeof(json), 0, 'F'), &b2));
    static const char BADCH[] = {'/', ':', '`', 'g', '@', 'G', ' ', '-', 'x', 'z'};
    static const int POS[] = {0, 1, 62, 63};
    for (size_t i = 0; i < sizeof(BADCH); i++) {
        for (size_t k = 0; k < sizeof(POS) / sizeof(POS[0]); k++) {
            expect_begin_bad(sha_json(json, sizeof(json), POS[k], BADCH[i]));
        }
    }
}

void test_begin_sha256_length_and_type(void) {
    expect_begin_bad("{\"name\":\"n\",\"size\":34816,\"sha256\":\"\"}");
    expect_begin_bad("{\"name\":\"n\",\"size\":34816,\"sha256\":\"0000000000000000000000000000000000000000000000000000000000000000 \"}");
    expect_begin_bad("{\"name\":\"n\",\"size\":34816,\"sha256\":\"000000000000000000000000000000000000000000000000000000000000000\"}");   /* 63 */
    expect_begin_bad("{\"name\":\"n\",\"size\":34816,\"sha256\":\"00000000000000000000000000000000000000000000000000000000000000000\"}"); /* 65 */
    expect_begin_bad("{\"name\":\"n\",\"size\":34816,\"sha256\":null}");
    expect_begin_bad("{\"name\":\"n\",\"size\":34816,\"sha256\":0}");
    expect_begin_bad("{\"name\":\"n\",\"size\":34816,\"sha256\":true}");
    iso_api_body_t b;
    char json[256];
    snprintf(json, sizeof(json), "{\"name\":\"n\",\"size\":34816,\"sha256\":\"\\u0031%s\"}", "000000000000000000000000000000000000000000000000000000000000000"); /* 1 + 63 個の 0 */
    TEST_ASSERT_EQUAL_INT(0, parse(BEGIN, json, &b));
    TEST_ASSERT_TRUE(b.has_sha);
    TEST_ASSERT_EQUAL_UINT8(0x10, b.sha[0]);
}

/* ---- begin: 名前 ----------------------------------------------------------------------- */

void test_begin_name_utf8_and_escapes(void) {
    expect_begin_name("{\"name\":\"\xE6\x97\xA5\xE6\x9C\xAC.iso\",\"size\":34816}", "\xE6\x97\xA5\xE6\x9C\xAC.iso", 10); /* 日本 */
    expect_begin_name("{\"name\":\"caf\\u00e9\",\"size\":34816}", "caf\xC3\xA9", 5);
    expect_begin_name("{\"name\":\"caf\\u00E9\",\"size\":34816}", "caf\xC3\xA9", 5);
    expect_begin_name("{\"name\":\"\\u3042\",\"size\":34816}", "\xE3\x81\x82", 3);
    expect_begin_name("{\"name\":\"\\u0041b\",\"size\":34816}", "Ab", 2);
    expect_begin_name("{\"name\":\"q\\\"q\",\"size\":34816}", "q\"q", 3);
    expect_begin_name("{\"name\":\"q\\u0022q\",\"size\":34816}", "q\"q", 3);
    expect_begin_name("{\"name\":\"a b\",\"size\":34816}", "a b", 3);
    expect_begin_name("{\"name\":\"a\\u0020b\",\"size\":34816}", "a b", 3);
    expect_begin_name("{\"name\":\"\\u00a0\",\"size\":34816}", "\xC2\xA0", 2); /* NBSP は C1 ではない */
    expect_begin_name("{\"name\":\"\\u007e\",\"size\":34816}", "~", 1);
    /* 境界の符号位置 */
    expect_begin_name("{\"name\":\"\\u00a0\\u07ff\\u0800\",\"size\":34816}", "\xC2\xA0\xDF\xBF\xE0\xA0\x80", 7);
    expect_begin_name("{\"name\":\"\\ud7ff\",\"size\":34816}", "\xED\x9F\xBF", 3);
    expect_begin_name("{\"name\":\"\\ue000\",\"size\":34816}", "\xEE\x80\x80", 3);
    expect_begin_name("{\"name\":\"\\uffff\",\"size\":34816}", "\xEF\xBF\xBF", 3);
    /* サロゲートペア */
    expect_begin_name("{\"name\":\"\\ud83d\\ude00\",\"size\":34816}", "\xF0\x9F\x98\x80", 4);
    expect_begin_name("{\"name\":\"\\uD83D\\uDE00\",\"size\":34816}", "\xF0\x9F\x98\x80", 4);
    expect_begin_name("{\"name\":\"\\ud800\\udc00\",\"size\":34816}", "\xF0\x90\x80\x80", 4);     /* U+10000 */
    expect_begin_name("{\"name\":\"\\udbff\\udfff\",\"size\":34816}", "\xF4\x8F\xBF\xBF", 4);     /* U+10FFFF */
    expect_begin_name("{\"name\":\"\\ud800\\udfff\",\"size\":34816}", "\xF0\x90\x8F\xBF", 4);     /* U+103FF */
    expect_begin_name("{\"name\":\"\\udbff\\udc00\",\"size\":34816}", "\xF4\x8F\xB0\x80", 4);     /* U+10FC00 */
    expect_begin_name("{\"name\":\"x\\ud83d\\ude00y\",\"size\":34816}", "x\xF0\x9F\x98\x80y", 6);
    /* 生の 4 バイトの UTF-8 */
    expect_begin_name("{\"name\":\"\xF0\x9F\x98\x80\",\"size\":34816}", "\xF0\x9F\x98\x80", 4);
}

void test_begin_name_forbidden_characters_after_decoding(void) {
    expect_begin_bad("{\"name\":\"\\u0000\",\"size\":34816}");
    expect_begin_bad("{\"name\":\"a\\u0000b\",\"size\":34816}");
    expect_begin_bad("{\"name\":\"a\\u0001b\",\"size\":34816}");
    expect_begin_bad("{\"name\":\"a\\u001fb\",\"size\":34816}"); /* C0 の最後 */
    expect_begin_bad("{\"name\":\"a\\u007fb\",\"size\":34816}"); /* DEL */
    expect_begin_bad("{\"name\":\"a\\u0080b\",\"size\":34816}"); /* C1 の最初 */
    expect_begin_bad("{\"name\":\"a\\u009fb\",\"size\":34816}"); /* C1 の最後 */
    expect_begin_bad("{\"name\":\"a\\u002fb\",\"size\":34816}"); /* '/' */
    expect_begin_bad("{\"name\":\"a\\u005cb\",\"size\":34816}"); /* '\' */
    expect_begin_bad("{\"name\":\"a\\/b\",\"size\":34816}");
    expect_begin_bad("{\"name\":\"a\\\\b\",\"size\":34816}");
    expect_begin_bad("{\"name\":\"a\\nb\",\"size\":34816}");
    expect_begin_bad("{\"name\":\"a\\tb\",\"size\":34816}");
    expect_begin_bad("{\"name\":\"a\\rb\",\"size\":34816}");
    expect_begin_bad("{\"name\":\"a\\bb\",\"size\":34816}");
    expect_begin_bad("{\"name\":\"a\\fb\",\"size\":34816}");
    expect_begin_bad("{\"name\":\"a/b\",\"size\":34816}");
    expect_begin_bad("{\"name\":\" a\",\"size\":34816}");  /* 前の空白 */
    expect_begin_bad("{\"name\":\"a \",\"size\":34816}");  /* 後ろの空白 */
    expect_begin_bad("{\"name\":\"\\u0020a\",\"size\":34816}");
    expect_begin_bad("{\"name\":\"a\\u0020\",\"size\":34816}");
    expect_begin_bad("{\"name\":\" \",\"size\":34816}");
    expect_begin_bad("{\"name\":\"\",\"size\":34816}");     /* 空 */
    /* 生の不正な UTF-8 は、名前の規則(iso_name_valid)で拒否する */
    expect_begin_bad("{\"name\":\"a\xFF\",\"size\":34816}");
    expect_begin_bad("{\"name\":\"\xC0\x80\",\"size\":34816}");      /* 過長な表現 */
    expect_begin_bad("{\"name\":\"\xC1\xBF\",\"size\":34816}");
    expect_begin_bad("{\"name\":\"\xED\xA0\x80\",\"size\":34816}");  /* サロゲートの符号化 */
    expect_begin_bad("{\"name\":\"\xF4\x90\x80\x80\",\"size\":34816}"); /* U+110000 */
    expect_begin_bad("{\"name\":\"\xC3\",\"size\":34816}");          /* 途中で終わる */
    expect_begin_bad("{\"name\":\"\x80\",\"size\":34816}");          /* 継続バイトが先頭 */
    expect_begin_bad("{\"name\":\"\xC2\x80\",\"size\":34816}");      /* 生の C1 制御文字 */
    expect_begin_bad("{\"name\":\"a\x7f\",\"size\":34816}");         /* 生の DEL */
}

void test_begin_name_raw_control_characters_are_not_json(void) {
    expect_begin_bad("{\"name\":\"a\x01z\",\"size\":34816}");
    expect_begin_bad("{\"name\":\"a\tz\",\"size\":34816}");
    expect_begin_bad("{\"name\":\"a\nz\",\"size\":34816}");
    expect_begin_bad("{\"name\":\"a\rz\",\"size\":34816}");
    expect_begin_bad("{\"name\":\"a\x1f" "z\",\"size\":34816}");
    iso_api_body_t b;
    const char nul[] = "{\"name\":\"a\0z\",\"size\":34816}"; /* 生の NUL */
    TEST_ASSERT_EQUAL_INT(-1, iso_api_parse_body(BEGIN, nul, sizeof(nul) - 1, &b));
}

void test_begin_name_surrogate_errors(void) {
    expect_begin_bad("{\"name\":\"\\ud800\",\"size\":34816}");           /* 上位だけ */
    expect_begin_bad("{\"name\":\"\\udbff\",\"size\":34816}");
    expect_begin_bad("{\"name\":\"\\udc00\",\"size\":34816}");           /* 下位だけ */
    expect_begin_bad("{\"name\":\"\\udfff\",\"size\":34816}");
    expect_begin_bad("{\"name\":\"\\ud800\\u0041\",\"size\":34816}");    /* 上位 + 下位でない */
    expect_begin_bad("{\"name\":\"\\ud800\\ud800\",\"size\":34816}");    /* 上位 + 上位 */
    expect_begin_bad("{\"name\":\"\\ud800\\ue000\",\"size\":34816}");    /* 下位サロゲートの範囲の外 */
    expect_begin_bad("{\"name\":\"\\ud800\\udbff\",\"size\":34816}");
    expect_begin_bad("{\"name\":\"\\ud800x\",\"size\":34816}");
    expect_begin_bad("{\"name\":\"\\ud800\\\\u\",\"size\":34816}");
    expect_begin_bad("{\"name\":\"\\ud800\\n\",\"size\":34816}");
    expect_begin_bad("{\"name\":\"\\ud800\\u00\",\"size\":34816}");
    expect_begin_bad("{\"name\":\"\\ud800\\u\",\"size\":34816}");
    expect_begin_bad("{\"name\":\"\\udc00\\ud800\",\"size\":34816}");    /* 下位 + 上位 */
    expect_begin_bad("{\"name\":\"\\ud83d\\ude0\",\"size\":34816}");     /* 下位が 3 桁 */
    expect_begin_bad("{\"name\":\"\\ud800\\udc0g\",\"size\":34816}");
}

void test_begin_name_escape_syntax_errors(void) {
    expect_begin_bad("{\"name\":\"\\x41\",\"size\":34816}");
    expect_begin_bad("{\"name\":\"\\a\",\"size\":34816}");
    expect_begin_bad("{\"name\":\"\\U0041\",\"size\":34816}");
    expect_begin_bad("{\"name\":\"\\u12\",\"size\":34816}");
    expect_begin_bad("{\"name\":\"\\u123\",\"size\":34816}");
    expect_begin_bad("{\"name\":\"\\u12g4\",\"size\":34816}");
    expect_begin_bad("{\"name\":\"\\ug123\",\"size\":34816}");
    expect_begin_bad("{\"name\":\"\\u 123\",\"size\":34816}");
    expect_begin_bad("{\"name\":\"\\u\",\"size\":34816}");
    expect_begin_bad("{\"name\":\"abc\\");   /* 末尾が '\' */
    expect_begin_bad("{\"name\":\"abc\\u");
    expect_begin_bad("{\"name\":\"abc\\u00");
    expect_begin_bad("{\"name\":\"abc\\u004");
    expect_begin_bad("{\"name\":\"abc");       /* 閉じていない */
    expect_begin_bad("{\"name\":\"abc\",\"size\":34816"); /* 閉じていない */
    expect_begin_bad("{\"name\":abc,\"size\":34816}");
    expect_begin_bad("{\"name\":'abc',\"size\":34816}");
    expect_begin_bad("{\"name\":5,\"size\":34816}");
    expect_begin_bad("{\"name\":null,\"size\":34816}");
    expect_begin_bad("{\"name\":true,\"size\":34816}");
    expect_begin_bad("{\"name\":[\"a\"],\"size\":34816}");
    expect_begin_bad("{\"name\":{\"a\":1},\"size\":34816}");
}

void test_begin_name_length_limit_is_63_bytes_after_decoding(void) {
    char json[512];
    char name[80];
    memset(name, 'n', 63);
    name[63] = '\0';
    snprintf(json, sizeof(json), "{\"name\":\"%s\",\"size\":34816}", name);
    expect_begin_name(json, name, 63);
    memset(name, 'n', 64);
    name[64] = '\0';
    snprintf(json, sizeof(json), "{\"name\":\"%s\",\"size\":34816}", name);
    expect_begin_bad(json);
    /* エスケープで 63 バイト: 62 バイト + 1 つの `\u0041` */
    memset(name, 'n', 62);
    name[62] = '\0';
    snprintf(json, sizeof(json), "{\"name\":\"%s\\u0041\",\"size\":34816}", name);
    char want[80];
    memset(want, 'n', 62);
    want[62] = 'A';
    expect_begin_name(json, want, 63);
    /* 63 バイトの境界を、3 バイトの符号位置がまたぐ: 61 バイト + 3 バイト = 64 */
    memset(name, 'n', 61);
    name[61] = '\0';
    snprintf(json, sizeof(json), "{\"name\":\"%s\\u3042\",\"size\":34816}", name);
    expect_begin_bad(json);
    /* 60 バイト + 3 バイト = 63 */
    memset(name, 'n', 60);
    name[60] = '\0';
    snprintf(json, sizeof(json), "{\"name\":\"%s\\u3042\",\"size\":34816}", name);
    memset(want, 'n', 60);
    memcpy(want + 60, "\xE3\x81\x82", 3);
    expect_begin_name(json, want, 63);
    /* 4 バイトの符号位置: 60 + 4 = 64 */
    snprintf(json, sizeof(json), "{\"name\":\"%s\\ud83d\\ude00\",\"size\":34816}", name);
    expect_begin_bad(json);
    memset(name, 'n', 59);
    name[59] = '\0';
    snprintf(json, sizeof(json), "{\"name\":\"%s\\ud83d\\ude00\",\"size\":34816}", name);
    memset(want, 'n', 59);
    memcpy(want + 59, "\xF0\x9F\x98\x80", 4);
    expect_begin_name(json, want, 63);
    /* 2 バイトの符号位置: 62 + 2 = 64 */
    memset(name, 'n', 62);
    name[62] = '\0';
    snprintf(json, sizeof(json), "{\"name\":\"%s\\u00e9\",\"size\":34816}", name);
    expect_begin_bad(json);
    memset(name, 'n', 61);
    name[61] = '\0';
    snprintf(json, sizeof(json), "{\"name\":\"%s\\u00e9\",\"size\":34816}", name);
    memset(want, 'n', 61);
    memcpy(want + 61, "\xC3\xA9", 2);
    expect_begin_name(json, want, 63);
}

/* ---- begin: size ---------------------------------------------------------------------- */

void test_begin_size_range(void) {
    iso_api_body_t b;
    TEST_ASSERT_EQUAL_INT(0, parse(BEGIN, "{\"name\":\"a\",\"size\":34816}", &b));
    TEST_ASSERT_EQUAL_UINT64(34816, b.size);
    expect_begin_bad("{\"name\":\"a\",\"size\":34815}");
    TEST_ASSERT_EQUAL_INT(0, parse(BEGIN, "{\"name\":\"a\",\"size\":34817}", &b));
    TEST_ASSERT_EQUAL_INT(0, parse(BEGIN, "{\"name\":\"a\",\"size\":17179869184}", &b)); /* 16 GiB */
    TEST_ASSERT_EQUAL_UINT64(17179869184ull, b.size);
    TEST_ASSERT_EQUAL_INT(0, parse(BEGIN, "{\"name\":\"a\",\"size\":17179869183}", &b));
    expect_begin_bad("{\"name\":\"a\",\"size\":17179869185}");
    expect_begin_bad("{\"name\":\"a\",\"size\":0}");
    expect_begin_bad("{\"name\":\"a\",\"size\":1}");
    expect_begin_bad("{\"name\":\"a\",\"size\":18446744073709551615}");
    expect_begin_bad("{\"name\":\"a\",\"size\":18446744073709551616}");
}

void test_begin_size_must_be_a_plain_integer(void) {
    expect_begin_bad("{\"name\":\"a\",\"size\":\"34816\"}");
    expect_begin_bad("{\"name\":\"a\",\"size\":34816.0}");
    expect_begin_bad("{\"name\":\"a\",\"size\":3.4816e4}");
    expect_begin_bad("{\"name\":\"a\",\"size\":34816e0}");
    expect_begin_bad("{\"name\":\"a\",\"size\":34816E0}");
    expect_begin_bad("{\"name\":\"a\",\"size\":-34816}");
    expect_begin_bad("{\"name\":\"a\",\"size\":+34816}");
    expect_begin_bad("{\"name\":\"a\",\"size\":034816}");
    expect_begin_bad("{\"name\":\"a\",\"size\":0x8800}");
    expect_begin_bad("{\"name\":\"a\",\"size\":34 816}");
    expect_begin_bad("{\"name\":\"a\",\"size\":34_816}");
    expect_begin_bad("{\"name\":\"a\",\"size\":true}");
    expect_begin_bad("{\"name\":\"a\",\"size\":null}");
    expect_begin_bad("{\"name\":\"a\",\"size\":[34816]}");
    expect_begin_bad("{\"name\":\"a\",\"size\":{}}");
    expect_begin_bad("{\"name\":\"a\",\"size\":}");
    expect_begin_bad("{\"name\":\"a\",\"size\":,}");
    expect_begin_bad("{\"name\":\"a\",\"size\":34816,}");
}

/* ---- begin: 構造 ---------------------------------------------------------------------- */

void test_begin_structure_errors(void) {
    expect_begin_bad("");
    expect_begin_bad(" ");
    expect_begin_bad("{}");
    expect_begin_bad("{\"name\":\"a\"}");          /* size がない */
    expect_begin_bad("{\"size\":34816}");          /* name がない */
    expect_begin_bad("{\"sha256\":\"0000000000000000000000000000000000000000000000000000000000000000\"}");
    expect_begin_bad("{\"name\":\"a\",\"name\":\"b\",\"size\":34816}");   /* 重複 */
    expect_begin_bad("{\"name\":\"a\",\"size\":34816,\"size\":34816}");
    expect_begin_bad("{\"name\":\"a\",\"size\":34816,\"sha256\":\"0000000000000000000000000000000000000000000000000000000000000000\",\"sha256\":\"0000000000000000000000000000000000000000000000000000000000000000\"}");
    expect_begin_bad("{\"name\":\"a\",\"size\":34816,\"force\":true}");   /* 定義していないキー */
    expect_begin_bad("{\"name\":\"a\",\"size\":34816,\"id\":\"a1b2c3d4\"}");
    expect_begin_bad("{\"name\":\"a\",\"size\":34816,\"cursor\":1}");
    expect_begin_bad("{\"Name\":\"a\",\"size\":34816}");
    expect_begin_bad("{\"nam\":\"a\",\"size\":34816}");
    expect_begin_bad("{\"names\":\"a\",\"size\":34816}");
    expect_begin_bad("{\"\":\"a\",\"name\":\"a\",\"size\":34816}");
    expect_begin_bad("{\"name\\u0000\":\"a\",\"size\":34816}");
    expect_begin_bad("{\"aaaaaaaaaaaaaaaaaaaaaaaaa\":1,\"name\":\"a\",\"size\":34816}"); /* 長いキー */
    expect_begin_bad("{\"name\":\"a\",\"size\":34816,}");
    expect_begin_bad("{,\"name\":\"a\",\"size\":34816}");
    expect_begin_bad("{\"name\":\"a\",,\"size\":34816}");
    expect_begin_bad("{\"name\":\"a\" \"size\":34816}");
    expect_begin_bad("{\"name\" \"a\",\"size\":34816}");
    expect_begin_bad("{\"name\"=\"a\",\"size\":34816}");
    expect_begin_bad("{name:\"a\",\"size\":34816}");
    expect_begin_bad("{\"name\":\"a\",\"size\":34816");
    expect_begin_bad("\"name\":\"a\",\"size\":34816}");
    expect_begin_bad("[{\"name\":\"a\",\"size\":34816}]");
    expect_begin_bad("[]");
    expect_begin_bad("\"x\"");
    expect_begin_bad("1");
    expect_begin_bad("null");
    expect_begin_bad("true");
    expect_begin_bad("{\"name\":\"a\",\"size\":34816} x");
    expect_begin_bad("{\"name\":\"a\",\"size\":34816}x");
    expect_begin_bad("{\"name\":\"a\",\"size\":34816}{}");
    expect_begin_bad("{\"name\":\"a\",\"size\":34816}}");
    expect_begin_bad("{\"name\":\"a\",\"size\":34816}\x01");
    expect_begin_bad("{{\"name\":\"a\",\"size\":34816}}");
    expect_begin_bad("\xEF\xBB\xBF{\"name\":\"a\",\"size\":34816}"); /* BOM */
}

void test_begin_a_nul_byte_after_the_object_is_rejected(void) {
    iso_api_body_t b;
    const char j[] = "{\"name\":\"a\",\"size\":34816}\0";
    TEST_ASSERT_EQUAL_INT(0, iso_api_parse_body(BEGIN, j, sizeof(j) - 2, &b));
    TEST_ASSERT_EQUAL_INT(-1, iso_api_parse_body(BEGIN, j, sizeof(j) - 1, &b));
    const char k[] = "{\"name\":\"a\",\"size\":34816} \0 ";
    TEST_ASSERT_EQUAL_INT(-1, iso_api_parse_body(BEGIN, k, sizeof(k) - 1, &b));
    const char l[] = "{\"name\":\"a\",\"size\":3\0" "4816}";
    TEST_ASSERT_EQUAL_INT(-1, iso_api_parse_body(BEGIN, l, sizeof(l) - 1, &b));
}

void test_the_body_is_at_most_512_bytes(void) {
    char json[700];
    iso_api_body_t b;
    /* 512 バイトちょうど: 空白で埋める */
    const char *core = "{\"name\":\"a\",\"size\":34816}";
    size_t n = strlen(core);
    memcpy(json, core, n);
    memset(json + n, ' ', 512 - n);
    TEST_ASSERT_EQUAL_INT(0, iso_api_parse_body(BEGIN, json, 512, &b));
    memset(json + n, ' ', 513 - n);
    TEST_ASSERT_EQUAL_INT(-1, iso_api_parse_body(BEGIN, json, 513, &b));
    TEST_ASSERT_EQUAL_INT(-1, iso_api_parse_body(BEGIN, json, 600, &b));
    /* ほかのスキーマでも */
    memset(json, ' ', 600);
    memcpy(json, "{}", 2);
    TEST_ASSERT_EQUAL_INT(0, iso_api_parse_body(FINISH, json, 512, &b));
    TEST_ASSERT_EQUAL_INT(-1, iso_api_parse_body(FINISH, json, 513, &b));
    TEST_ASSERT_EQUAL_INT(-1, iso_api_parse_body(RESCAN, json, 513, &b));
    TEST_ASSERT_EQUAL_INT(-1, iso_api_parse_body(EJECT, json, 513, &b));
    TEST_ASSERT_EQUAL_INT(-1, iso_api_parse_body(MOUNT, json, 513, &b));
    /* NULL の本体は、長さ 0 のときだけ */
    TEST_ASSERT_EQUAL_INT(0, iso_api_parse_body(FINISH, NULL, 0, &b));
    TEST_ASSERT_EQUAL_INT(-1, iso_api_parse_body(FINISH, NULL, 2, &b));
}

/* ---- mount ----------------------------------------------------------------------------- */

void test_mount_body(void) {
    iso_api_body_t b;
    TEST_ASSERT_EQUAL_INT(0, parse(MOUNT, "{\"id\":\"a1b2c3d4\"}", &b));
    TEST_ASSERT_EQUAL_STRING("a1b2c3d4", b.id);
    TEST_ASSERT_FALSE(b.force);
    TEST_ASSERT_EQUAL_INT(0, parse(MOUNT, "{\"id\":\"a1b2c3d4\",\"force\":true}", &b));
    TEST_ASSERT_TRUE(b.force);
    TEST_ASSERT_EQUAL_INT(0, parse(MOUNT, "{\"id\":\"a1b2c3d4\",\"force\":false}", &b));
    TEST_ASSERT_FALSE(b.force);
    TEST_ASSERT_EQUAL_INT(0, parse(MOUNT, " { \"force\" : true , \"id\" : \"00000000\" } ", &b));
    TEST_ASSERT_TRUE(b.force);
    TEST_ASSERT_EQUAL_STRING("00000000", b.id);
    TEST_ASSERT_EQUAL_INT(0, parse(MOUNT, "{\"id\":\"\\u0061\\u0031b2c3d4\"}", &b)); /* 復号してから検査する */
    TEST_ASSERT_EQUAL_STRING("a1b2c3d4", b.id);
}

void test_mount_body_errors(void) {
    iso_api_body_t b;
    static const char *BAD[] = {
        "",                                          /* 本体が必要 */
        "{}",                                        /* id が必要 */
        "{\"force\":true}",
        "{\"id\":\"A1B2C3D4\"}",                     /* 小文字だけ */
        "{\"id\":\"a1b2c3d\"}",                      /* 7 文字 */
        "{\"id\":\"a1b2c3d45\"}",                    /* 9 文字 */
        "{\"id\":\"a1b2c3dg\"}",
        "{\"id\":\"\"}",
        "{\"id\":null}",
        "{\"id\":12345678}",
        "{\"id\":\"a1b2c3d4 \"}",
        "{\"id\":\"a1b2c3d4\",\"id\":\"a1b2c3d4\"}", /* 重複 */
        "{\"id\":\"a1b2c3d4\",\"force\":true,\"force\":true}",
        "{\"id\":\"a1b2c3d4\",\"force\":\"true\"}",
        "{\"id\":\"a1b2c3d4\",\"force\":1}",
        "{\"id\":\"a1b2c3d4\",\"force\":null}",
        "{\"id\":\"a1b2c3d4\",\"force\":True}",
        "{\"id\":\"a1b2c3d4\",\"force\":tru}",
        "{\"id\":\"a1b2c3d4\",\"force\":truee}",
        "{\"id\":\"a1b2c3d4\",\"force\":fals}",
        "{\"id\":\"a1b2c3d4\",\"name\":\"x\"}",      /* 定義していないキー */
        "{\"id\":\"a1b2c3d4\",\"cursor\":1}",
        "{\"id\":\"a1b2c3d4\",\"sha256\":\"x\"}",
        "{\"id\":\"a1b2c3d4\"",
        "{\"id\":\"a1b2c3d4\"} x",
        " ",
    };
    for (size_t i = 0; i < sizeof(BAD) / sizeof(BAD[0]); i++) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(-1, parse(MOUNT, BAD[i], &b), BAD[i]);
    }
}

/* ---- eject / rescan / finish ----------------------------------------------------------------- */

void test_eject_body(void) {
    iso_api_body_t b;
    TEST_ASSERT_EQUAL_INT(0, iso_api_parse_body(EJECT, NULL, 0, &b)); /* 本体なしは {} */
    TEST_ASSERT_FALSE(b.force);
    TEST_ASSERT_EQUAL_INT(0, parse(EJECT, "{}", &b));
    TEST_ASSERT_FALSE(b.force);
    TEST_ASSERT_EQUAL_INT(0, parse(EJECT, " { } ", &b));
    TEST_ASSERT_EQUAL_INT(0, parse(EJECT, "{\"force\":true}", &b));
    TEST_ASSERT_TRUE(b.force);
    TEST_ASSERT_EQUAL_INT(0, parse(EJECT, "{\"force\":false}", &b));
    TEST_ASSERT_FALSE(b.force);
    static const char *BAD[] = {" ", "{\"id\":\"a1b2c3d4\"}", "{\"force\":\"true\"}", "{\"force\":true,\"force\":true}", "{\"force\":1}",
                                "{\"cursor\":1}", "{\"force\":true,}", "[]", "{\"force\":true} x", "{"};
    for (size_t i = 0; i < sizeof(BAD) / sizeof(BAD[0]); i++) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(-1, parse(EJECT, BAD[i], &b), BAD[i]);
    }
}

void test_rescan_body(void) {
    iso_api_body_t b;
    TEST_ASSERT_EQUAL_INT(0, iso_api_parse_body(RESCAN, NULL, 0, &b));
    TEST_ASSERT_EQUAL_UINT32(0, b.cursor);
    TEST_ASSERT_EQUAL_INT(0, parse(RESCAN, "{}", &b));
    TEST_ASSERT_EQUAL_UINT32(0, b.cursor);
    TEST_ASSERT_EQUAL_INT(0, parse(RESCAN, "{\"cursor\":0}", &b));
    TEST_ASSERT_EQUAL_UINT32(0, b.cursor);
    TEST_ASSERT_EQUAL_INT(0, parse(RESCAN, "{\"cursor\":4}", &b));
    TEST_ASSERT_EQUAL_UINT32(4, b.cursor);
    TEST_ASSERT_EQUAL_INT(0, parse(RESCAN, "{\"cursor\":65534}", &b));
    TEST_ASSERT_EQUAL_UINT32(65534, b.cursor);
    TEST_ASSERT_EQUAL_INT(0, parse(RESCAN, "{\"cursor\":65535}", &b));
    TEST_ASSERT_EQUAL_UINT32(65535, b.cursor);
    static const char *BAD[] = {"{\"cursor\":65536}", "{\"cursor\":-1}", "{\"cursor\":1.5}", "{\"cursor\":\"4\"}", "{\"cursor\":null}",
                                "{\"cursor\":00}", "{\"cursor\":04}", "{\"cursor\":}", "{\"cursor\":4,\"cursor\":4}", "{\"force\":true}",
                                "{\"cursor\":4294967296}", "{\"cursor\":18446744073709551616}", " ", "{\"cursor\":1e1}", "[4]"};
    for (size_t i = 0; i < sizeof(BAD) / sizeof(BAD[0]); i++) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(-1, parse(RESCAN, BAD[i], &b), BAD[i]);
    }
}

void test_finish_body(void) {
    iso_api_body_t b;
    TEST_ASSERT_EQUAL_INT(0, iso_api_parse_body(FINISH, NULL, 0, &b));
    TEST_ASSERT_EQUAL_INT(0, parse(FINISH, "{}", &b));
    TEST_ASSERT_EQUAL_INT(0, parse(FINISH, "\r\n{ }\r\n", &b));
    static const char *BAD[] = {"{\"a\":1}", "{\"force\":true}", "{\"id\":\"a1b2c3d4\"}", "[]", " ", "{", "{}}", "{} {}", "{\"\":1}"};
    for (size_t i = 0; i < sizeof(BAD) / sizeof(BAD[0]); i++) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(-1, parse(FINISH, BAD[i], &b), BAD[i]);
    }
}

void test_the_output_is_cleared_first(void) {
    iso_api_body_t b;
    memset(&b, 0xCC, sizeof(b));
    TEST_ASSERT_EQUAL_INT(0, iso_api_parse_body(RESCAN, "{}", 2, &b));
    TEST_ASSERT_EQUAL_UINT32(0, b.cursor);
    TEST_ASSERT_FALSE(b.force);
    TEST_ASSERT_FALSE(b.has_sha);
    TEST_ASSERT_EQUAL_UINT64(0, b.size);
    TEST_ASSERT_EQUAL_UINT(0, b.name_len);
    memset(&b, 0xCC, sizeof(b));
    TEST_ASSERT_EQUAL_INT(0, iso_api_parse_body(EJECT, NULL, 0, &b));
    TEST_ASSERT_FALSE(b.force);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_begin_minimal_and_full);
    RUN_TEST(test_begin_whitespace_and_key_order);
    RUN_TEST(test_begin_sha256_accepts_either_case);
    RUN_TEST(test_begin_sha256_length_and_type);
    RUN_TEST(test_begin_name_utf8_and_escapes);
    RUN_TEST(test_begin_name_forbidden_characters_after_decoding);
    RUN_TEST(test_begin_name_raw_control_characters_are_not_json);
    RUN_TEST(test_begin_name_surrogate_errors);
    RUN_TEST(test_begin_name_escape_syntax_errors);
    RUN_TEST(test_begin_name_length_limit_is_63_bytes_after_decoding);
    RUN_TEST(test_begin_size_range);
    RUN_TEST(test_begin_size_must_be_a_plain_integer);
    RUN_TEST(test_begin_structure_errors);
    RUN_TEST(test_begin_a_nul_byte_after_the_object_is_rejected);
    RUN_TEST(test_the_body_is_at_most_512_bytes);
    RUN_TEST(test_mount_body);
    RUN_TEST(test_mount_body_errors);
    RUN_TEST(test_eject_body);
    RUN_TEST(test_rescan_body);
    RUN_TEST(test_finish_body);
    RUN_TEST(test_the_output_is_cleared_first);
    return UNITY_END();
}
