/*
 * iso_catalog: 名前の規則(設計 7、WEB_API 8)と <id> の形式。期待値は手書き。
 */
#include "ic_common.h"

void setUp(void) {}
void tearDown(void) {}

typedef struct {
    const char *bytes;
    size_t len;
    bool valid;
    const char *why;
} name_case_t;

#define S(lit) lit, sizeof(lit) - 1

void test_name_rule_table(void) {
    static const name_case_t cs[] = {
        /* 長さ */
        {S("a"), true, "1 byte"},
        {S(""), false, "empty"},
        {S("ubuntu-24.04"), true, "typical"},
        {S("0123456789012345678901234567890123456789012345678901234567890123"), false, "64 bytes"},
        {S("012345678901234567890123456789012345678901234567890123456789012"), true, "63 bytes"},
        /* 前後の空白 (U+0020) */
        {S(" a"), false, "leading space"},
        {S("a "), false, "trailing space"},
        {S(" "), false, "only a space"},
        {S("a b"), true, "inner space"},
        {S("a  b"), true, "two inner spaces"},
        {S("\xE3\x80\x80x"), true, "leading U+3000 (ideographic space) is not U+0020"},
        {S("x\xE3\x80\x80"), true, "trailing U+3000"},
        {S("\xC2\xA0x"), true, "leading U+00A0 (no-break space)"},
        /* C0 */
        {S("a\x01" "b"), false, "U+0001"},
        {S("a\x1F" "b"), false, "U+001F"},
        {S("a\x09" "b"), false, "tab"},
        {S("a\x0A" "b"), false, "newline"},
        {S("a\x0D" "b"), false, "carriage return"},
        {S("a\x00" "b"), false, "embedded NUL"},
        {S("\x00"), false, "only NUL"},
        {S("a\x20" "b"), true, "U+0020 inside"},
        /* DEL */
        {S("a\x7F" "b"), false, "DEL"},
        {S("a\x7E" "b"), true, "tilde"},
        /* / と \ */
        {S("a/b"), false, "slash"},
        {S("a\\b"), false, "backslash"},
        {S("/"), false, "only slash"},
        {S(".."), true, "dots are fine (the path never uses the name)"},
        {S("a:b*c?"), true, "other punctuation"},
        /* C1 (U+0080〜009F) */
        {S("a\xC2\x80" "b"), false, "U+0080"},
        {S("a\xC2\x85" "b"), false, "U+0085 (NEL)"},
        {S("a\xC2\x9F" "b"), false, "U+009F"},
        {S("a\xC2\xA0" "b"), true, "U+00A0"},
        /* UTF-8 の境界: 2 バイト */
        {S("\xC3\xA9"), true, "U+00E9"},
        {S("\xDF\xBF"), true, "U+07FF"},
        {S("\xC0\x80"), false, "overlong NUL"},
        {S("\xC0\xAF"), false, "overlong slash"},
        {S("\xC1\xBF"), false, "overlong U+007F"},
        /* 3 バイト */
        {S("\xE0\xA0\x80"), true, "U+0800"},
        {S("\xE0\x9F\xBF"), false, "overlong U+07FF"},
        {S("\xE0\x80\x80"), false, "overlong"},
        {S("\xED\x9F\xBF"), true, "U+D7FF"},
        {S("\xED\xA0\x80"), false, "U+D800 (surrogate)"},
        {S("\xED\xAF\xBF"), false, "U+DBFF (surrogate)"},
        {S("\xED\xB0\x80"), false, "U+DC00 (surrogate)"},
        {S("\xED\xBF\xBF"), false, "U+DFFF (surrogate)"},
        {S("\xEE\x80\x80"), true, "U+E000"},
        {S("\xEF\xBF\xBF"), true, "U+FFFF"},
        /* 4 バイト */
        {S("\xF0\x90\x80\x80"), true, "U+10000"},
        {S("\xF0\x8F\xBF\xBF"), false, "overlong U+FFFF"},
        {S("\xF0\x80\x80\x80"), false, "overlong"},
        {S("\xF4\x8F\xBF\xBF"), true, "U+10FFFF"},
        {S("\xF4\x90\x80\x80"), false, "U+110000"},
        {S("\xF5\x80\x80\x80"), false, "F5 lead"},
        {S("\xF8\x88\x80\x80\x80"), false, "5-byte form"},
        {S("\xFC\x84\x80\x80\x80\x80"), false, "6-byte form"},
        {S("\xFE"), false, "FE"},
        {S("\xFF"), false, "FF"},
        /* 壊れた並び */
        {S("\x80"), false, "lone continuation"},
        {S("\xBF"), false, "lone continuation BF"},
        {S("a\x80"), false, "stray continuation after ASCII"},
        {S("\xC3\xA9\xA9"), false, "extra continuation"},
        {S("\xC3"), false, "truncated 2-byte"},
        {S("\xE3\x82"), false, "truncated 3-byte"},
        {S("\xF0\x90\x80"), false, "truncated 4-byte"},
        {S("\xC3\x41"), false, "bad continuation (ASCII)"},
        {S("\xE3\x82\x41"), false, "bad 3rd byte"},
        {S("\xE3\x41\x82"), false, "bad 2nd byte"},
        {S("\xF0\x90\x80\x41"), false, "bad 4th byte"},
        {S("\xC3\xC3"), false, "lead after lead"},
        {S("\xE3\xE3\x82"), false, "lead in the middle"},
        /* 日本語 */
        {S("\xE3\x82\xA4\xE3\x83\xA1\xE3\x83\xBC\xE3\x82\xB8"), true, "katakana: ime-ji"},
        {S("\xF0\x9F\x92\xBF"), true, "emoji (4 bytes)"},
        /* 3 バイトの文字で 63 / 64 バイト */
        {S("\xE3\x81\x82\xE3\x81\x82\xE3\x81\x82\xE3\x81\x82\xE3\x81\x82\xE3\x81\x82\xE3\x81\x82\xE3\x81\x82\xE3\x81\x82\xE3\x81\x82"
           "\xE3\x81\x82\xE3\x81\x82\xE3\x81\x82\xE3\x81\x82\xE3\x81\x82\xE3\x81\x82\xE3\x81\x82\xE3\x81\x82\xE3\x81\x82\xE3\x81\x82\xE3\x81\x82"),
         true, "21 x 3 bytes = 63"},
        {S("\xE3\x81\x82\xE3\x81\x82\xE3\x81\x82\xE3\x81\x82\xE3\x81\x82\xE3\x81\x82\xE3\x81\x82\xE3\x81\x82\xE3\x81\x82\xE3\x81\x82"
           "\xE3\x81\x82\xE3\x81\x82\xE3\x81\x82\xE3\x81\x82\xE3\x81\x82\xE3\x81\x82\xE3\x81\x82\xE3\x81\x82\xE3\x81\x82\xE3\x81\x82\xE3\x81\x82"
           "a"),
         false, "21 x 3 + 1 = 64"},
    };
    for (size_t i = 0; i < sizeof(cs) / sizeof(cs[0]); i++) {
        char msg[120];
        snprintf(msg, sizeof(msg), "case %zu: %s", i, cs[i].why);
        TEST_ASSERT_EQUAL_INT_MESSAGE(cs[i].valid ? 1 : 0, iso_name_valid(cs[i].bytes, cs[i].len) ? 1 : 0, msg);
    }
}

void test_name_valid_checks_only_len_bytes(void) {
    /* len が NUL を含むバイト数。バッファの後ろの内容は見ない */
    const char buf[] = "abc\x01xyz";
    TEST_ASSERT_TRUE(iso_name_valid(buf, 3));
    TEST_ASSERT_FALSE(iso_name_valid(buf, 4));
    TEST_ASSERT_FALSE(iso_name_valid(NULL, 3));
    TEST_ASSERT_FALSE(iso_name_valid(buf, 0));
    /* 末尾の空白は len の最後のバイトで判定する */
    const char sp[] = "ab  ";
    TEST_ASSERT_TRUE(iso_name_valid(sp, 2));
    TEST_ASSERT_FALSE(iso_name_valid(sp, 3));
    /* 先頭の空白 */
    const char lead[] = " ab";
    TEST_ASSERT_FALSE(iso_name_valid(lead, 3));
    TEST_ASSERT_TRUE(iso_name_valid(lead + 1, 2));
}

void test_name_valid_every_ascii_byte_in_the_middle(void) {
    for (int c = 0; c < 128; c++) {
        char s[3] = {'x', (char)c, 'y'};
        bool expect = (c >= 0x20 && c != 0x7F && c != '/' && c != '\\');
        char msg[40];
        snprintf(msg, sizeof(msg), "byte 0x%02X", c);
        TEST_ASSERT_EQUAL_INT_MESSAGE(expect ? 1 : 0, iso_name_valid(s, 3) ? 1 : 0, msg);
    }
}

void test_name_valid_length_boundary_with_ascii(void) {
    char buf[80];
    memset(buf, 'k', sizeof(buf));
    for (size_t n = 0; n <= 70; n++) {
        bool expect = (n >= 1 && n <= 63);
        TEST_ASSERT_EQUAL_INT(expect ? 1 : 0, iso_name_valid(buf, n) ? 1 : 0);
    }
}

void test_name_equal_is_exact_bytes(void) {
    TEST_ASSERT_TRUE(iso_name_equal("abc", 3, "abc", 3));
    TEST_ASSERT_FALSE(iso_name_equal("abc", 3, "abd", 3));
    TEST_ASSERT_FALSE(iso_name_equal("abc", 3, "ABC", 3)); /* 大文字小文字を区別する */
    TEST_ASSERT_FALSE(iso_name_equal("abc", 3, "abcd", 4));
    TEST_ASSERT_FALSE(iso_name_equal("abcd", 4, "abc", 3));
    TEST_ASSERT_FALSE(iso_name_equal("abc", 3, "ab", 2));
    TEST_ASSERT_TRUE(iso_name_equal("", 0, "", 0));
    TEST_ASSERT_TRUE(iso_name_equal("abc", 2, "abd", 2)); /* 比べるのは len バイトだけ */
    /* 正規化はしない: U+00E9 (C3 A9) と e + U+0301 (65 CC 81) は別 */
    TEST_ASSERT_FALSE(iso_name_equal("\xC3\xA9", 2, "e\xCC\x81", 3));
    /* 最後のバイトだけの違い */
    TEST_ASSERT_FALSE(iso_name_equal("abcdefgh", 8, "abcdefgi", 8));
    /* 最初のバイトだけの違い */
    TEST_ASSERT_FALSE(iso_name_equal("xbcdefgh", 8, "abcdefgh", 8));
}

void test_id_format(void) {
    TEST_ASSERT_TRUE(iso_id_valid("a1b2c3d4"));
    TEST_ASSERT_TRUE(iso_id_valid("00000000"));
    TEST_ASSERT_TRUE(iso_id_valid("ffffffff"));
    TEST_ASSERT_TRUE(iso_id_valid("09afaf90"));
    TEST_ASSERT_FALSE(iso_id_valid("A1B2C3D4"));  /* 大文字は不可 */
    TEST_ASSERT_FALSE(iso_id_valid("a1b2c3dD"));  /* 一部が大文字 */
    TEST_ASSERT_FALSE(iso_id_valid("a1b2c3d"));   /* 7 桁 */
    TEST_ASSERT_FALSE(iso_id_valid("a1b2c3d45")); /* 9 桁 */
    TEST_ASSERT_FALSE(iso_id_valid(""));
    TEST_ASSERT_FALSE(iso_id_valid("a1b2c3dg"));  /* 'g' */
    TEST_ASSERT_FALSE(iso_id_valid("g1b2c3d4"));
    TEST_ASSERT_FALSE(iso_id_valid("a1b2c3d/"));
    TEST_ASSERT_FALSE(iso_id_valid("a1b2c3d "));
    TEST_ASSERT_FALSE(iso_id_valid(" 1b2c3d4"));
    TEST_ASSERT_FALSE(iso_id_valid("import"));
    TEST_ASSERT_FALSE(iso_id_valid("a1b2c3d@")); /* '@' ('A' - 1) */
    TEST_ASSERT_FALSE(iso_id_valid("a1b2c3d`")); /* '`' ('a' - 1) */
    TEST_ASSERT_FALSE(iso_id_valid("a1b2c3d:")); /* ':' ('9' + 1) */
    TEST_ASSERT_FALSE(iso_id_valid("a1b2c3d\x2F")); /* '/' ('0' - 1) */
    TEST_ASSERT_FALSE(iso_id_valid("a1b2c3dg")); /* 'g' ('f' + 1) */
    TEST_ASSERT_FALSE(iso_id_valid(NULL));
    /* 8 桁の途中に NUL */
    const char z[9] = {'a', '1', 'b', '2', 0, '3', 'd', '4', 0};
    TEST_ASSERT_FALSE(iso_id_valid(z));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_name_rule_table);
    RUN_TEST(test_name_valid_checks_only_len_bytes);
    RUN_TEST(test_name_valid_every_ascii_byte_in_the_middle);
    RUN_TEST(test_name_valid_length_boundary_with_ascii);
    RUN_TEST(test_name_equal_is_exact_bytes);
    RUN_TEST(test_id_format);
    return UNITY_END();
}
