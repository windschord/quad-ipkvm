/*
 * http_guard_video_accept のテスト (docs/VIDEO_TRANSPORT.md 9.4。P7)。
 * /video の URI のクエリ (`?` の後ろ) から、accept の値 (カンマ区切り。未知の値は無視) を解析する純 C。
 * すべての入力は、query_len ちょうどのヒープ領域 (NUL なし) に置いて呼ぶ (ASan が、1 バイトの範囲外の読みも検出する)。
 */
#include "unity.h"
#include "http_guard.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

/* query を、NUL なし・ちょうどの長さのヒープにコピーして呼ぶ */
static uint32_t acc_n(const char *query, size_t n) {
    char *p = (char *)malloc(n ? n : 1);
    TEST_ASSERT_NOT_NULL(p);
    if (n) memcpy(p, query, n);
    uint32_t r = http_guard_video_accept(p, n);
    free(p);
    return r;
}

static uint32_t acc(const char *query) { return acc_n(query, strlen(query)); }

typedef struct {
    const char *query;
    uint32_t expect;
    const char *why;
} row_t;

static void run_table(const row_t *t, size_t n) {
    for (size_t i = 0; i < n; i++) {
        char msg[300];
        snprintf(msg, sizeof(msg), "row %zu: query=[%s] (%s)", i, t[i].query, t[i].why);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(t[i].expect, acc(t[i].query), msg);
    }
}

#define N(a) (sizeof(a) / sizeof((a)[0]))
#define H HTTP_GUARD_VIDEO_ACCEPT_H264

void test_constants(void) {
    TEST_ASSERT_EQUAL_UINT32(1u, HTTP_GUARD_VIDEO_ACCEPT_H264);
    TEST_ASSERT_EQUAL_UINT32(512u, HTTP_GUARD_VIDEO_QUERY_MAX);
}

void test_basic_h264(void) {
    static const row_t t[] = {
        {"accept=h264", H, "基本形"},
        {"accept=h264,", H, "末尾のカンマ"},
        {",accept=h264", 0, "先頭のカンマはキーの一部 (キーは ',accept')"},
        {"accept=,h264", H, "先頭の空の要素は無視"},
        {"accept=h264,h264", H, "同じ要素の繰り返し"},
        {"accept=h264,,,h264", H, "連続するカンマ"},
        {"accept=h265,h264", H, "未知の値の後ろ"},
        {"accept=h264,h265", H, "未知の値の前"},
        {"accept=av1,h264,vp9", H, "未知の値に挟まれる"},
        {"accept=a,b,c,h264,d", H, "多数の要素"},
    };
    run_table(t, N(t));
}

void test_no_accept_is_mjpeg_only(void) {
    static const row_t t[] = {
        {"", 0, "空 (長さ 0 は別のテストでも確認)"},
        {"accept", 0, "= がない"},
        {"accept=", 0, "値が空"},
        {"accept=,", 0, "空の要素だけ"},
        {"accept=,,,", 0, "空の要素だけ (複数)"},
        {"foo=bar", 0, "ほかのキーだけ"},
        {"foo=h264", 0, "ほかのキーに h264"},
        {"h264", 0, "キーなしの h264"},
        {"h264=accept", 0, "キーと値の取り違え"},
        {"=h264", 0, "キーが空"},
        {"&", 0, "区切りだけ"},
        {"&&&", 0, "区切りだけ (複数)"},
        {"=", 0, "= だけ"},
        {"accept=mjpeg", 0, "mjpeg は JPEG の別名ではない (accept に JPEG は書かない契約)"},
        {"accept=jpeg", 0, "jpeg"},
    };
    run_table(t, N(t));
}

void test_unknown_values_ignored(void) {
    static const row_t t[] = {
        {"accept=h265", 0, "未知のコーデック"},
        {"accept=av1", 0, "未知のコーデック"},
        {"accept=h26", 0, "h264 の接頭辞 (短い)"},
        {"accept=h2644", 0, "h264 に続く 1 文字"},
        {"accept=h2645", 0, "h264 に続く数字"},
        {"accept=xh264", 0, "h264 の前に 1 文字"},
        {"accept=h264x", 0, "h264 の後ろに 1 文字"},
        {"accept=h264;", 0, "; は区切りではない"},
        {"accept=h264;q=0.5", 0, "品質値 (; は区切りではない)。値の全体が未知"},
        {"accept=h264 ", 0, "末尾の空白は未知"},
        {"accept= h264", 0, "先頭の空白は未知"},
        {"accept=h264\t", 0, "末尾のタブは未知"},
        {"accept=h-264", 0, "ハイフン"},
        {"accept=h.264", 0, "ドット"},
        {"accept=264", 0, "数字だけ"},
        {"accept=h", 0, "1 文字"},
        {"accept=4", 0, "1 文字"},
        {"accept=h26,h265,h2", 0, "h264 でない h26x だけ"},
    };
    run_table(t, N(t));
}

void test_case_sensitive(void) {
    static const row_t t[] = {
        {"accept=H264", 0, "大文字の値は未知"},
        {"accept=h264,H264", H, "小文字があれば真"},
        {"accept=H264,h264", H, "小文字があれば真 (順序)"},
        {"accept=h264", H, "小文字"},
        {"accept=h264,H", H, "ほかの要素が大文字 1 文字でも、h264 があれば真"},
        {"Accept=h264", 0, "キーの大文字は別のキー"},
        {"ACCEPT=h264", 0, "キーの大文字は別のキー"},
        {"aCcept=h264", 0, "キーの一部が大文字"},
        {"accepT=h264", 0, "キーの末尾が大文字"},
    };
    run_table(t, N(t));
}

void test_key_must_match_exactly(void) {
    static const row_t t[] = {
        {"xaccept=h264", 0, "前に 1 文字"},
        {"accept2=h264", 0, "後ろに 1 文字"},
        {"accepts=h264", 0, "複数形"},
        {"accep=h264", 0, "1 文字足りない"},
        {"acept=h264", 0, "綴りが違う"},
        {"_accept=h264", 0, "前に _"},
        {"accept_=h264", 0, "後ろに _"},
        {"?accept=h264", 0, "クエリの先頭の ? は呼び出し側が除く。残っていればキーの一部"},
        {"accept =h264", 0, "キーの後ろに空白"},
        {" accept=h264", 0, "キーの前に空白"},
        {"x.accept=h264", 0, "名前空間のような接頭辞"},
        {"accept.x=h264", 0, "名前空間のような接尾辞"},
        {"accepth264", 0, "= がない"},
        {"acceptt=h264", 0, "t が 1 つ多い"},
        {"aaccept=h264", 0, "a が 1 つ多い"},
        {"ccept=h264", 0, "先頭が足りない"},
    };
    run_table(t, N(t));
}

void test_multiple_keys_and_pairs(void) {
    static const row_t t[] = {
        {"foo=1&accept=h264", H, "前に別のキー"},
        {"accept=h264&foo=1", H, "後ろに別のキー"},
        {"a=1&b=2&accept=h264&c=3", H, "間に挟まる"},
        {"accept=h264&accept=h264", H, "同じ値が 2 回"},
        {"&accept=h264", H, "先頭の &"},
        {"accept=h264&", H, "末尾の &"},
        {"&&accept=h264&&", H, "前後の連続する &"},
        {"x&accept=h264", H, "= のないキーの後ろ"},
        {"accept=h264&x", H, "= のないキーの前"},
        {"accept=&accept=h264", H, "(OR) 1 つ目が空、2 つ目が h264"},
        {"accept=h264&accept=", H, "(OR) 1 つ目が h264、2 つ目が空"},
        {"accept=h265&accept=h264", H, "(OR) 1 つ目が未知、2 つ目が h264"},
        {"accept=h264&accept=h265", H, "(OR) 1 つ目が h264、2 つ目が未知"},
        {"accept=h265&accept=av1", 0, "(OR) どちらも h264 なし"},
        {"accept=&accept=", 0, "(OR) どちらも空"},
        {"accept=h264&xaccept=h265&accept=av1", H, "別のキーを挟んでも、h264 があれば真"},
        {"xaccept=h264&accept=h265", 0, "h264 は別のキーにしかない"},
        {"accept=h265&xaccept=h264", 0, "h264 は別のキーにしかない (順序)"},
        {"foo=accept=h264", 0, "値に = があっても、キーは foo"},
        {"accept=a=h264", 0, "値の中の = は値の一部 (a=h264 は未知)"},
        {"accept=h264=1", 0, "値の中の = は値の一部 (h264=1 は未知)"},
        {"accept=h264&&&accept=x", H, "連続する &"},
        {"a=b=c&accept=h264", H, "前の組の値に = が複数"},
        {"accept=x&accept=y&accept=h264", H, "3 つ目"},
        {"accept=x&accept=y&accept=z", 0, "3 つとも h264 なし"},
    };
    run_table(t, N(t));
}

void test_no_decoding(void) {
    static const row_t t[] = {
        {"accept=%68264", 0, "%68 = h だが、解釈しない"},
        {"accept=h%32%36%34", 0, "%32%36%34 = 264 だが、解釈しない"},
        {"accept=h264%00", 0, "%00 は文字として未知"},
        {"accept=h%2C264", 0, "%2C = , だが、解釈しない"},
        {"accept=h264%2Cx", 0, "%2C = , だが、区切りにならない (h264%2Cx は未知)"},
        {"%61ccept=h264", 0, "%61 = a だが、キーは解釈しない"},
        {"accept%3Dh264", 0, "%3D = = だが、解釈しない"},
        {"foo=1%26accept=h264", 0, "%26 = & だが、区切りにならない"},
        {"accept=h264+", 0, "+ は空白ではなく、未知の文字"},
        {"accept=h+264", 0, "+ は未知の文字"},
        {"accept=h264#", 0, "# は未知の文字 (フラグメントは呼び出し側が除く)"},
        {"accept=h264?", 0, "? は未知の文字"},
        {"accept=h264/", 0, "/ は未知の文字"},
        {"accept=h264\\", 0, "バックスラッシュ"},
        {"accept=h264\"", 0, "引用符"},
        {"accept=\"h264\"", 0, "引用符で囲んだ値は未知"},
    };
    run_table(t, N(t));
}

void test_nonascii_bytes(void) {
    static const unsigned char a[] = {'a', 'c', 'c', 'e', 'p', 't', '=', 0xFF, ',', 'h', '2', '6', '4'};
    static const unsigned char b[] = {'a', 'c', 'c', 'e', 'p', 't', '=', 'h', '2', '6', '4', 0xC3};
    static const unsigned char c[] = {0xFF, '&', 'a', 'c', 'c', 'e', 'p', 't', '=', 'h', '2', '6', '4'};
    static const unsigned char d[] = {'a', 'c', 'c', 'e', 'p', 't', '=', 'h', '2', '6', '4', ',', 0x80, 0x81};
    TEST_ASSERT_EQUAL_UINT32(H, acc_n((const char *)a, sizeof(a)));
    TEST_ASSERT_EQUAL_UINT32(0, acc_n((const char *)b, sizeof(b)));
    TEST_ASSERT_EQUAL_UINT32(H, acc_n((const char *)c, sizeof(c)));
    TEST_ASSERT_EQUAL_UINT32(H, acc_n((const char *)d, sizeof(d)));
}

void test_null_and_zero_length(void) {
    TEST_ASSERT_EQUAL_UINT32(0, http_guard_video_accept(NULL, 0));
    TEST_ASSERT_EQUAL_UINT32(0, http_guard_video_accept(NULL, 11));
    TEST_ASSERT_EQUAL_UINT32(0, http_guard_video_accept(NULL, (size_t)-1));
    /* 長さ 0: query が有効でも読まない (NULL でない、長さ 0 のヒープ。ASan が読みを検出する) */
    char *p = (char *)malloc(1);
    TEST_ASSERT_NOT_NULL(p);
    p[0] = 'x';
    TEST_ASSERT_EQUAL_UINT32(0, http_guard_video_accept(p, 0));
    free(p);
    TEST_ASSERT_EQUAL_UINT32(0, http_guard_video_accept("accept=h264", 0));
}

void test_length_bounds_not_read_past(void) {
    /* query_len が文字列より短いとき、その先 (ここでは "h264") は読まない */
    TEST_ASSERT_EQUAL_UINT32(0, acc_n("accept=h264", 10)); /* "accept=h26" */
    TEST_ASSERT_EQUAL_UINT32(H, acc_n("accept=h264", 11));
    TEST_ASSERT_EQUAL_UINT32(0, acc_n("accept=h264", 7));  /* "accept=" */
    TEST_ASSERT_EQUAL_UINT32(0, acc_n("accept=h264", 6));  /* "accept" */
    TEST_ASSERT_EQUAL_UINT32(0, acc_n("accept=h264", 1));
    /* 長さちょうどで、直後に別の文字がある場合 (h264 の後ろ) も、長さの外は見ない */
    TEST_ASSERT_EQUAL_UINT32(H, acc_n("accept=h2645", 11)); /* 切って "accept=h264" */
    TEST_ASSERT_EQUAL_UINT32(0, acc_n("accept=h2645", 12));
    TEST_ASSERT_EQUAL_UINT32(H, acc_n("accept=h264&x", 11));
    TEST_ASSERT_EQUAL_UINT32(H, acc_n("accept=h264,x", 11));
    /* 切ったために、キーが accept でなくなる */
    TEST_ASSERT_EQUAL_UINT32(0, acc_n("accept=h264", 5));
}

void test_embedded_nul_returns_zero(void) {
    static const char a[] = "accept=h264\0junk";   /* NUL の前に h264 があっても、NUL を含むので 0 (安全側) */
    static const char b[] = "foo=1\0accept=h264";  /* NUL の後ろに accept */
    static const char c[] = "accept=h2\0""64";     /* NUL が値の途中 */
    static const char d[] = "\0accept=h264";       /* NUL が先頭 */
    static const char e[] = "accept=h264\0";       /* NUL が末尾 (長さに含める) */
    TEST_ASSERT_EQUAL_UINT32(0, acc_n(a, sizeof(a) - 1));
    TEST_ASSERT_EQUAL_UINT32(0, acc_n(b, sizeof(b) - 1));
    TEST_ASSERT_EQUAL_UINT32(0, acc_n(c, sizeof(c) - 1));
    TEST_ASSERT_EQUAL_UINT32(0, acc_n(d, sizeof(d) - 1));
    TEST_ASSERT_EQUAL_UINT32(0, acc_n(e, sizeof(e) - 1));
    /* NUL を含まない範囲 (長さで NUL の手前まで) なら、通常どおり */
    TEST_ASSERT_EQUAL_UINT32(H, acc_n(e, sizeof(e) - 2));
    TEST_ASSERT_EQUAL_UINT32(H, acc_n(a, 11));
    /* 長さ 1 の NUL だけ */
    TEST_ASSERT_EQUAL_UINT32(0, acc_n("", 1));
}

void test_max_length_boundary(void) {
    char *buf = (char *)malloc(HTTP_GUARD_VIDEO_QUERY_MAX + 2);
    TEST_ASSERT_NOT_NULL(buf);
    const char *tail = "&accept=h264";
    size_t tl = strlen(tail);

    /* 長さちょうど MAX: 解析する */
    memset(buf, 'x', HTTP_GUARD_VIDEO_QUERY_MAX);
    memcpy(buf + HTTP_GUARD_VIDEO_QUERY_MAX - tl, tail, tl);
    TEST_ASSERT_EQUAL_UINT32(H, http_guard_video_accept(buf, HTTP_GUARD_VIDEO_QUERY_MAX));

    /* MAX - 1 (末尾を 1 つ削る: accept=h26 になる): 0 */
    TEST_ASSERT_EQUAL_UINT32(0, http_guard_video_accept(buf, HTTP_GUARD_VIDEO_QUERY_MAX - 1));

    /* MAX + 1: 解析しない (h264 があっても 0)。先頭に accept=h264 を置く */
    memset(buf, 'x', HTTP_GUARD_VIDEO_QUERY_MAX + 1);
    memcpy(buf, "accept=h264&", 12);
    TEST_ASSERT_EQUAL_UINT32(H, http_guard_video_accept(buf, HTTP_GUARD_VIDEO_QUERY_MAX));
    TEST_ASSERT_EQUAL_UINT32(0, http_guard_video_accept(buf, HTTP_GUARD_VIDEO_QUERY_MAX + 1));

    /* 極端な長さ: 範囲外を読まずに 0。ここでは、実際には小さい領域を指す (読むと ASan が検出する) */
    TEST_ASSERT_EQUAL_UINT32(0, http_guard_video_accept(buf, (size_t)-1));
    TEST_ASSERT_EQUAL_UINT32(0, http_guard_video_accept(buf, (size_t)1 << 40));
    TEST_ASSERT_EQUAL_UINT32(0, http_guard_video_accept(buf, HTTP_GUARD_VIDEO_QUERY_MAX + 1000));
    free(buf);
}

void test_long_values_inside_limit(void) {
    /* 値が長くても (上限の中で) 正しく解析する */
    char *buf = (char *)malloc(HTTP_GUARD_VIDEO_QUERY_MAX);
    TEST_ASSERT_NOT_NULL(buf);
    size_t n = 0;
    memcpy(buf, "accept=", 7);
    n = 7;
    while (n + 5 < HTTP_GUARD_VIDEO_QUERY_MAX - 4) {
        memcpy(buf + n, "xyz,", 4);
        n += 4;
    }
    TEST_ASSERT_EQUAL_UINT32(0, http_guard_video_accept(buf, n));
    memcpy(buf + n, "h264", 4);
    TEST_ASSERT_EQUAL_UINT32(H, http_guard_video_accept(buf, n + 4));
    free(buf);
}

void test_return_is_only_known_bits(void) {
    /* 返すのは、既知のビットだけ (将来、ビットが増えても、いまは H264 だけ) */
    static const char *qs[] = {"accept=h264", "accept=h264&accept=h264", "", "x", "accept=zzz"};
    for (size_t i = 0; i < N(qs); i++) {
        uint32_t r = acc(qs[i]);
        TEST_ASSERT_EQUAL_UINT32(r & HTTP_GUARD_VIDEO_ACCEPT_H264, r);
    }
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_constants);
    RUN_TEST(test_basic_h264);
    RUN_TEST(test_no_accept_is_mjpeg_only);
    RUN_TEST(test_unknown_values_ignored);
    RUN_TEST(test_case_sensitive);
    RUN_TEST(test_key_must_match_exactly);
    RUN_TEST(test_multiple_keys_and_pairs);
    RUN_TEST(test_no_decoding);
    RUN_TEST(test_nonascii_bytes);
    RUN_TEST(test_null_and_zero_length);
    RUN_TEST(test_length_bounds_not_read_past);
    RUN_TEST(test_embedded_nul_returns_zero);
    RUN_TEST(test_max_length_boundary);
    RUN_TEST(test_long_values_inside_limit);
    RUN_TEST(test_return_is_only_known_bits);
    return UNITY_END();
}
