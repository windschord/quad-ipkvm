/*
 * http_guard_origin_ok_for_port のテスト (docs/VIDEO_TRANSPORT.md 5 章「Origin の規則」)。
 * 映像専用の httpd (別ポート) 用: Origin はページのポート (メイン)、Host は映像サーバーのポート。
 */
#include "unity.h"
#include "http_guard.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>

void setUp(void) {}
void tearDown(void) {}

static bool chk(const char *origin, const char *host, uint16_t page_port) {
    return http_guard_origin_ok_for_port(origin, strlen(origin), host, strlen(host), page_port);
}

typedef struct {
    const char *origin;
    const char *host;
    uint16_t page_port;
    bool expect;
    const char *why;
} row_t;

static void run_table(const row_t *t, size_t n) {
    for (size_t i = 0; i < n; i++) {
        char msg[300];
        snprintf(msg, sizeof(msg), "row %zu: origin=[%s] host=[%s] page_port=%u (%s)", i, t[i].origin, t[i].host,
                 (unsigned)t[i].page_port, t[i].why);
        TEST_ASSERT_EQUAL_MESSAGE(t[i].expect, chk(t[i].origin, t[i].host, t[i].page_port), msg);
    }
}

#define N(a) (sizeof(a) / sizeof((a)[0]))

void test_accept_main_port_443(void) {
    static const row_t t[] = {
        {"https://kvm.local", "kvm.local:8444", 443, true, "Origin にポートなし = 443、Host は映像のポート"},
        {"https://kvm.local:443", "kvm.local:8444", 443, true, "page_port == 443 のとき :443 があってもよい"},
        {"https://192.168.1.100", "192.168.1.100:8444", 443, true, "IPv4"},
        {"https://192.168.1.100:443", "192.168.1.100:8444", 443, true, "IPv4 と :443"},
        {"https://kvm.local", "kvm.local", 443, true, "Host にポートなし"},
        {"https://kvm.local", "kvm.local:443", 443, true, "Host のポートは値を問わない"},
        {"https://kvm.local", "kvm.local:1", 443, true, "Host のポートの最小"},
        {"https://kvm.local", "kvm.local:65535", 443, true, "Host のポートの最大"},
        {"https://a", "a:8444", 443, true, "1 文字のホスト名"},
        {"https://my-kvm_1.example.com", "my-kvm_1.example.com:8444", 443, true, "- と _ と数字"},
    };
    run_table(t, N(t));
}

void test_accept_nonstandard_main_port(void) {
    static const row_t t[] = {
        {"https://kvm.local:8443", "kvm.local:8444", 8443, true, "メインが 8443"},
        {"https://192.168.1.100:8443", "192.168.1.100:8444", 8443, true, "IPv4"},
        {"https://kvm.local:1", "kvm.local:8444", 1, true, "最小のポート"},
        {"https://kvm.local:65535", "kvm.local:8444", 65535, true, "最大のポート"},
        {"https://kvm.local:8443", "kvm.local", 8443, true, "Host にポートなし"},
        {"https://kvm.local:8444", "kvm.local:8444", 8444, true, "メインと映像が同じポート (呼び出し側の設定次第)"},
        {"https://kvm.local:80", "kvm.local:8444", 80, true, "page_port 80 でも、値が一致すれば受理"},
    };
    run_table(t, N(t));
}

void test_port_mismatch(void) {
    static const row_t t[] = {
        {"https://kvm.local:8443", "kvm.local:8444", 443, false, "page_port 443 で Origin が :8443"},
        {"https://kvm.local", "kvm.local:8444", 8443, false, "page_port 8443 で Origin にポートなし (= 443)"},
        {"https://kvm.local:443", "kvm.local:8444", 8443, false, "page_port 8443 で Origin が :443"},
        {"https://kvm.local:8444", "kvm.local:8444", 443, false, "Origin が映像のポート (ページは映像のポートではない)"},
        {"https://kvm.local:8442", "kvm.local:8444", 8443, false, "1 違い (小)"},
        {"https://kvm.local:8444", "kvm.local:8444", 8443, false, "1 違い (大)"},
        {"https://kvm.local:44", "kvm.local:8444", 443, false, "先頭の一致だけ"},
        {"https://kvm.local:4430", "kvm.local:8444", 443, false, "接頭辞の一致だけ"},
        {"https://kvm.local", "kvm.local:8444", 0, false, "page_port == 0"},
        {"https://kvm.local:0", "kvm.local:8444", 0, false, "Origin のポート 0"},
        {"https://kvm.local:65535", "kvm.local:8444", 65534, false, "最大 - 1"},
        {"https://kvm.local:1", "kvm.local:8444", 2, false, "最小 + 1"},
        {"https://kvm.local:443", "kvm.local:8444", 442, false, "page_port がずれている"},
    };
    run_table(t, N(t));
}

void test_host_name_mismatch(void) {
    static const row_t t[] = {
        {"https://evil.com", "kvm.local:8444", 443, false, "別のホスト"},
        {"https://kvm.local.evil.com", "kvm.local:8444", 443, false, "Host が Origin の接頭辞"},
        {"https://evil.kvm.local", "kvm.local:8444", 443, false, "Host が Origin の接尾辞"},
        {"https://kvm.loca", "kvm.local:8444", 443, false, "Origin が短い"},
        {"https://kvm.locall", "kvm.local:8444", 443, false, "Origin が長い"},
        {"https://kvm.localX", "kvm.local:8444", 443, false, "末尾に 1 文字"},
        {"https://Xkvm.local", "kvm.local:8444", 443, false, "先頭に 1 文字"},
        {"https://kvm-local", "kvm.local:8444", 443, false, "文字の違い (. と -)"},
        {"https://192.168.1.100", "192.168.1.101:8444", 443, false, "IP の最後の桁"},
        {"https://192.168.1.10", "192.168.1.100:8444", 443, false, "IP の桁数"},
        {"https://kvm.loca1", "kvm.local:8444", 443, false, "最後の 1 文字だけ違う"},
        {"https://xvm.local", "kvm.local:8444", 443, false, "最初の 1 文字だけ違う"},
        {"https://kvm.local", "kvm.local.evil.com:8444", 443, false, "Origin が Host の接頭辞"},
    };
    run_table(t, N(t));
}

void test_case_insensitive(void) {
    static const row_t t[] = {
        {"https://KVM.Local", "kvm.local:8444", 443, true, "Origin が大文字"},
        {"https://kvm.local", "KVM.LOCAL:8444", 443, true, "Host が大文字"},
        {"HTTPS://kvm.local", "kvm.local:8444", 443, true, "スキームが大文字"},
        {"HtTpS://kvm.local", "kvm.local:8444", 443, true, "スキームが混在"},
        {"hTtPs://KvM.lOcAl:8443", "kVm.LoCaL:8444", 8443, true, "すべて混在"},
    };
    run_table(t, N(t));
}

void test_reject_scheme(void) {
    static const row_t t[] = {
        {"http://kvm.local", "kvm.local:8444", 443, false, "http"},
        {"http://kvm.local:8443", "kvm.local:8444", 8443, false, "http とポート"},
        {"wss://kvm.local", "kvm.local:8444", 443, false, "wss"},
        {"ws://kvm.local", "kvm.local:8444", 443, false, "ws"},
        {"ftp://kvm.local", "kvm.local:8444", 443, false, "ftp"},
        {"https:/kvm.local", "kvm.local:8444", 443, false, "スラッシュが 1 本"},
        {"https:kvm.local", "kvm.local:8444", 443, false, "スラッシュなし"},
        {"https//kvm.local", "kvm.local:8444", 443, false, "コロンなし"},
        {"httpss://kvm.local", "kvm.local:8444", 443, false, "s が多い"},
        {"xhttps://kvm.local", "kvm.local:8444", 443, false, "接頭辞"},
        {" https://kvm.local", "kvm.local:8444", 443, false, "先頭の空白"},
        {"kvm.local", "kvm.local:8444", 443, false, "スキームなし"},
        {"//kvm.local", "kvm.local:8444", 443, false, "スキーム相対"},
        {"https://", "kvm.local:8444", 443, false, "ホストなし"},
        {"https:///", "kvm.local:8444", 443, false, "ホストなし + /"},
        {"https://:443", "kvm.local:8444", 443, false, "ホスト名が空 + ポート"},
        {"", "kvm.local:8444", 443, false, "空の Origin"},
        {"null", "kvm.local:8444", 443, false, "Origin: null"},
        {"NULL", "kvm.local:8444", 443, false, "Origin: NULL"},
        {"https://null", "null:8444", 443, true, "ホスト名が null (文字として有効)。null という Origin ではない"},
    };
    run_table(t, N(t));
}

void test_reject_userinfo_ipv6_trailing_dot_path(void) {
    static const row_t t[] = {
        {"https://admin@kvm.local", "kvm.local:8444", 443, false, "ユーザー情報"},
        {"https://kvm.local@evil.com", "kvm.local:8444", 443, false, "@ の前が一致 (userinfo の罠)"},
        {"https://evil.com@kvm.local", "kvm.local:8444", 443, false, "@ の後が一致"},
        {"https://@kvm.local", "kvm.local:8444", 443, false, "空のユーザー情報"},
        {"https://[::1]", "[::1]:8444", 443, false, "IPv6"},
        {"https://[::1]:8443", "[::1]:8444", 8443, false, "IPv6 とポート"},
        {"https://[::1]", "kvm.local:8444", 443, false, "Origin だけ IPv6"},
        {"https://kvm.local", "[::1]:8444", 443, false, "Host だけ IPv6"},
        {"https://kvm.local]", "kvm.local]:8444", 443, false, "] を含む"},
        {"https://kvm.local.", "kvm.local.:8444", 443, false, "末尾のドット (両方)"},
        {"https://kvm.local.", "kvm.local:8444", 443, false, "末尾のドット (Origin)"},
        {"https://kvm.local", "kvm.local.:8444", 443, false, "末尾のドット (Host)"},
        {"https://kvm.local.:8443", "kvm.local.:8444", 8443, false, "末尾のドットとポート"},
        {"https://.kvm.local", ".kvm.local:8444", 443, false, "先頭のドット"},
        {"https://kvm..local", "kvm..local:8444", 443, false, "連続したドット"},
        {"https://.", ".:8444", 443, false, "ドットだけ"},
        {"https://kvm.local/", "kvm.local:8444", 443, false, "末尾の /"},
        {"https://kvm.local/path", "kvm.local:8444", 443, false, "パス"},
        {"https://kvm.local:8443/", "kvm.local:8444", 8443, false, "ポートの後に /"},
        {"https://kvm.local/:8443", "kvm.local:8444", 8443, false, "/ の後のポート"},
        {"https://kvm.local?x=1", "kvm.local:8444", 443, false, "クエリー"},
        {"https://kvm.local#x", "kvm.local:8444", 443, false, "フラグメント"},
        {"https://kvm.local\\evil.com", "kvm.local:8444", 443, false, "バックスラッシュ"},
        {"https://kvm.local evil.com", "kvm.local:8444", 443, false, "空白"},
        {"https://kvm.local ", "kvm.local:8444", 443, false, "末尾の空白"},
        {"https://kvm.local\t", "kvm.local:8444", 443, false, "末尾のタブ"},
        {"https://kvm.local\r\n", "kvm.local:8444", 443, false, "CRLF"},
        {"https://kvm.local, https://evil.com", "kvm.local:8444", 443, false, "複数の Origin"},
        {"https://k\xc3\xb6m.local", "k\xc3\xb6m.local:8444", 443, false, "非 ASCII"},
        {"https://kvm.local%2eevil", "kvm.local%2eevil:8444", 443, false, "% エンコード"},
        {"https://kvm*local", "kvm*local:8444", 443, false, "ワイルドカード"},
    };
    run_table(t, N(t));
}

void test_reject_bad_ports(void) {
    static const row_t t[] = {
        {"https://kvm.local:", "kvm.local:8444", 443, false, "Origin のポートが空"},
        {"https://kvm.local:8443x", "kvm.local:8444", 8443, false, "Origin のポートに文字"},
        {"https://kvm.local:x8443", "kvm.local:8444", 8443, false, "Origin のポートに文字 (先頭)"},
        {"https://kvm.local:84 43", "kvm.local:8444", 8443, false, "Origin のポートに空白"},
        {"https://kvm.local:-1", "kvm.local:8444", 443, false, "負"},
        {"https://kvm.local:+443", "kvm.local:8444", 443, false, "+ つき"},
        {"https://kvm.local:08443", "kvm.local:8444", 8443, false, "先頭ゼロ"},
        {"https://kvm.local:0443", "kvm.local:8444", 443, false, "先頭ゼロ (443)"},
        {"https://kvm.local:00", "kvm.local:8444", 443, false, "00"},
        {"https://kvm.local:0", "kvm.local:8444", 443, false, "ポート 0"},
        {"https://kvm.local:65536", "kvm.local:8444", 443, false, "範囲外 (65536)"},
        {"https://kvm.local:99999", "kvm.local:8444", 443, false, "範囲外 (99999)"},
        {"https://kvm.local:100000", "kvm.local:8444", 443, false, "6 桁"},
        {"https://kvm.local:4294967739", "kvm.local:8444", 443, false, "2^32 + 443 (オーバーフローで 443 になってはいけない)"},
        {"https://kvm.local:18446744073709552059", "kvm.local:8444", 443, false, "2^64 + 443"},
        {"https://kvm.local:65779", "kvm.local:8444", 243, false, "65536 + 243 (16 ビットに切り詰めてはいけない)"},
        {"https://kvm.local:8443:8443", "kvm.local:8444", 8443, false, "ポートが 2 つ"},
        {"https://kvm.local::8443", "kvm.local:8444", 8443, false, "コロンが連続"},
        {"https://kvm.local", "kvm.local:", 443, false, "Host のポートが空"},
        {"https://kvm.local", "kvm.local:0", 443, false, "Host のポート 0"},
        {"https://kvm.local", "kvm.local:65536", 443, false, "Host のポートが範囲外"},
        {"https://kvm.local", "kvm.local:08444", 443, false, "Host のポートの先頭ゼロ"},
        {"https://kvm.local", "kvm.local:84x4", 443, false, "Host のポートに文字"},
        {"https://kvm.local", "kvm.local:8444:1", 443, false, "Host のポートが 2 つ"},
        {"https://kvm.local", "kvm.local:123456", 443, false, "Host のポートが 6 桁"},
        {"https://kvm.local", ":8444", 443, false, "Host のホスト名が空"},
    };
    run_table(t, N(t));
}

void test_reject_empty_and_null_inputs(void) {
    TEST_ASSERT_FALSE(http_guard_origin_ok_for_port(NULL, 0, "kvm.local", 9, 443));
    TEST_ASSERT_FALSE(http_guard_origin_ok_for_port(NULL, 17, "kvm.local", 9, 443));
    TEST_ASSERT_FALSE(http_guard_origin_ok_for_port("https://kvm.local", 17, NULL, 0, 443));
    TEST_ASSERT_FALSE(http_guard_origin_ok_for_port("https://kvm.local", 17, NULL, 9, 443));
    TEST_ASSERT_FALSE(http_guard_origin_ok_for_port("https://kvm.local", 0, "kvm.local", 9, 443));
    TEST_ASSERT_FALSE(http_guard_origin_ok_for_port("https://kvm.local", 17, "kvm.local", 0, 443));
    TEST_ASSERT_FALSE(http_guard_origin_ok_for_port("", 0, "", 0, 443));
    TEST_ASSERT_TRUE(http_guard_origin_ok_for_port("https://kvm.local", 17, "kvm.local", 9, 443)); // 基準
}

void test_length_arguments_are_respected(void) {
    // 長さの引数だけを見る (NUL 終端に頼らない)
    TEST_ASSERT_TRUE(http_guard_origin_ok_for_port("https://kvm.localEXTRA", 17, "kvm.local:8444EXTRA", 14, 443));
    TEST_ASSERT_FALSE(http_guard_origin_ok_for_port("https://kvm.localEXTRA", 18, "kvm.local:8444", 14, 443));
    TEST_ASSERT_TRUE(http_guard_origin_ok_for_port("https://kvm.local", 17, "kvm.local:8444", 13, 443));  // "kvm.local:844" (有効なポート。値は問わない)
    TEST_ASSERT_FALSE(http_guard_origin_ok_for_port("https://kvm.local", 17, "kvm.local:8444", 10, 443)); // "kvm.local:" (ポートが空)
    TEST_ASSERT_TRUE(http_guard_origin_ok_for_port("https://kvm.local", 17, "kvm.local:8444", 9, 443));   // ":8444" を長さで切る
    // 埋め込まれた NUL
    TEST_ASSERT_FALSE(http_guard_origin_ok_for_port("https://kvm.local\0x", 19, "kvm.local:8444", 14, 443));
    TEST_ASSERT_FALSE(http_guard_origin_ok_for_port("https://kvm.local", 17, "kvm.local\0:8444", 15, 443));
    TEST_ASSERT_FALSE(http_guard_origin_ok_for_port("https://kvm\0local", 17, "kvm\0local:8444", 14, 443));
}

void test_extreme_lengths(void) {
    static char origin[1100], host[1100];
    // ホスト名 255 バイト (許容の最大。"a" を 63 文字ずつ、'.' で区切る)
    char name[300];
    size_t n = 0;
    while (n < 255) {
        size_t lab = 63;
        if (n + lab > 255) lab = 255 - n;
        memset(name + n, 'a', lab);
        n += lab;
        if (n < 255) name[n++] = '.';
    }
    TEST_ASSERT_EQUAL(255, n);
    memcpy(origin, "https://", 8);
    memcpy(origin + 8, name, n);
    memcpy(host, name, n);
    TEST_ASSERT_TRUE(http_guard_origin_ok_for_port(origin, 8 + n, host, n, 443));
    memcpy(host + n, ":8444", 5);
    TEST_ASSERT_TRUE(http_guard_origin_ok_for_port(origin, 8 + n, host, n + 5, 443));
    memcpy(origin + 8 + n, ":8443", 5);
    TEST_ASSERT_TRUE(http_guard_origin_ok_for_port(origin, 8 + n + 5, host, n + 5, 8443));

    // 256 バイトのホスト名は拒否
    memcpy(origin, "https://", 8);
    memset(origin + 8, 'a', 256);
    memset(host, 'a', 256);
    TEST_ASSERT_FALSE(http_guard_origin_ok_for_port(origin, 8 + 256, host, 256, 443));
    TEST_ASSERT_FALSE(http_guard_origin_ok_for_port(origin, 8 + 256, host, 255, 443));
    TEST_ASSERT_FALSE(http_guard_origin_ok_for_port(origin, 8 + 255, host, 256, 443));

    // 1000 バイトのホスト名 / 長い Origin
    memset(origin, 'a', sizeof(origin));
    memcpy(origin, "https://", 8);
    memset(host, 'a', sizeof(host));
    TEST_ASSERT_FALSE(http_guard_origin_ok_for_port(origin, 1000, host, 992, 443));
    TEST_ASSERT_FALSE(http_guard_origin_ok_for_port(origin, 1000, host, 1000, 443));

    // 短い入力を、すべての長さで検査する (ASan で、範囲外の読み出しを検出する)
    const char *full = "https://a.b";
    for (size_t l = 0; l <= 11; l++) {
        char *copy = (char *)malloc(l ? l : 1);
        memcpy(copy, full, l);
        bool r = http_guard_origin_ok_for_port(copy, l, "a.b", 3, 443);
        TEST_ASSERT_EQUAL(l == 11, r);
        free(copy);
    }
    for (size_t l = 0; l <= 3; l++) {
        char *copy = (char *)malloc(l ? l : 1);
        memcpy(copy, "a.b", l);
        bool r = http_guard_origin_ok_for_port("https://a.b", 11, copy, l, 443);
        TEST_ASSERT_EQUAL(l == 3, r);
        free(copy);
    }
}

void test_name_of_255_with_trailing_dot(void) {
    // 255 バイトちょうどのホスト名の許容と、その末尾をドットにした場合の拒否
    static char o[600], h[600];
    memcpy(o, "https://", 8);
    memset(o + 8, 'b', 255);
    memset(h, 'b', 255);
    TEST_ASSERT_TRUE(http_guard_origin_ok_for_port(o, 8 + 255, h, 255, 443));
    o[8 + 254] = '.';
    h[254] = '.';
    TEST_ASSERT_FALSE(http_guard_origin_ok_for_port(o, 8 + 255, h, 255, 443));
}

/* 既存の http_guard_origin_ok と、同じ入力に対して矛盾しない */
void test_consistent_with_existing_origin_ok(void) {
    static const char *const hosts[] = {"192.168.1.100", "my-kvm.local", "a", "x.y.z"};
    static const char *const odd[] = {"https://", "null", "http://x", "https://a@b", "https://[::1]", "https://a.", "https://a/"};
    for (size_t i = 0; i < N(hosts); i++) {
        char o[100];
        snprintf(o, sizeof(o), "https://%s", hosts[i]);
        TEST_ASSERT_TRUE(http_guard_origin_ok(o, strlen(o), hosts[i], strlen(hosts[i])));
        TEST_ASSERT_TRUE(chk(o, hosts[i], 443));
        char h[100];
        snprintf(h, sizeof(h), "%s:8444", hosts[i]);
        TEST_ASSERT_TRUE(chk(o, h, 443));
        TEST_ASSERT_FALSE(http_guard_origin_ok(o, strlen(o), h, strlen(h))); // 既存の関数は、ポートが違うと拒否する
    }
    for (size_t i = 0; i < N(odd); i++) {
        TEST_ASSERT_FALSE(http_guard_origin_ok(odd[i], strlen(odd[i]), "a", 1));
        TEST_ASSERT_FALSE(chk(odd[i], "a:8444", 443));
        TEST_ASSERT_FALSE(chk(odd[i], "a", 443));
    }
}

/* 固定シードのファズ: 小さな文字集合の Origin / Host を作り、独立した実装
 * (文字列を分けて比べる単純な oracle) と一致すること */
static uint32_t rs = 1;
static uint32_t rnd(void) {
    rs ^= rs << 13;
    rs ^= rs >> 17;
    rs ^= rs << 5;
    return rs;
}

static bool oracle_chars_ok(const char *p) {
    for (; *p; p++) {
        bool ok = (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '-' || *p == '_' ||
                  *p == '.' || *p == ':';
        if (!ok) return false;
    }
    return true;
}

static bool oracle_port(const char *ps, long *out) {
    size_t pl = strlen(ps);
    if (pl == 0 || pl > 5 || (pl > 1 && ps[0] == '0')) return false;
    long v = 0;
    for (size_t i = 0; i < pl; i++) {
        if (ps[i] < '0' || ps[i] > '9') return false;
        v = v * 10 + (ps[i] - '0');
    }
    if (v < 1 || v > 65535) return false;
    *out = v;
    return true;
}

static bool oracle_name(const char *n, size_t len) {
    if (len == 0 || len > 255) return false;
    if (n[0] == '.' || n[len - 1] == '.') return false;
    for (size_t i = 0; i + 1 < len; i++) {
        if (n[i] == '.' && n[i + 1] == '.') return false;
    }
    return true;
}

static bool oracle(const char *o, size_t ol, const char *h, size_t hl, uint16_t pp) {
    char os[128], hs[128];
    if (ol >= sizeof(os) || hl >= sizeof(hs)) return false;
    memcpy(os, o, ol);
    os[ol] = 0;
    memcpy(hs, h, hl);
    hs[hl] = 0;
    if (strlen(os) != ol || strlen(hs) != hl) return false; // NUL を含む
    if (strncasecmp(os, "https://", 8) != 0) return false;
    const char *oa = os + 8;
    if (!oracle_chars_ok(oa) || !oracle_chars_ok(hs)) return false;
    const char *oc = strchr(oa, ':');
    const char *hc = strchr(hs, ':');
    size_t on = oc ? (size_t)(oc - oa) : strlen(oa);
    size_t hn = hc ? (size_t)(hc - hs) : strlen(hs);
    if (!oracle_name(oa, on) || !oracle_name(hs, hn)) return false;
    long op = 443, hp = 0;
    if (oc && !oracle_port(oc + 1, &op)) return false;
    if (hc && !oracle_port(hc + 1, &hp)) return false;
    if (op != (long)pp) return false;
    return on == hn && strncasecmp(oa, hs, on) == 0;
}

void test_fuzz_against_oracle(void) {
    static const char alpha[] = "aAbB0.-_:/@[]\\ ?#%\t9";
    static const char *const schemes[] = {"https://", "HTTPS://", "http://", "https:/", "", "null"};
    static const uint16_t pps[] = {443, 8443, 8444, 1, 65535, 0, 80};
    static const char *const ports[] = {"", ":443", ":8443", ":8444", ":0", ":65536", ":01", ":x", ":", ":65535", ":1", ":80"};
    long accepted = 0;
    rs = 0x9e3779b9u;
    for (int it = 0; it < 200000; it++) {
        char o[80], h[80];
        const char *sc = (rnd() % 10 < 8) ? schemes[rnd() % 2] : schemes[rnd() % 6];
        size_t sl = strlen(sc);
        memcpy(o, sc, sl);
        size_t ol = sl, hl = 0;
        // Origin と Host は、同じホスト名を共有する (一致する場合が出るように)
        char name[24];
        size_t nl = rnd() % 6;
        bool plain = (rnd() % 4 != 0);
        for (size_t i = 0; i < nl; i++) {
            name[i] = alpha[rnd() % (sizeof(alpha) - 1)];
            if (plain && (name[i] == ':' || name[i] == '/')) name[i] = 'a';
        }
        memcpy(o + ol, name, nl);
        ol += nl;
        memcpy(h, name, nl);
        hl = nl;
        if (rnd() % 8 == 0 && nl > 0) h[rnd() % nl] ^= 0x20; // 大小文字の差
        if (rnd() % 12 == 0 && nl > 0) h[0] = 'z';
        const char *op = ports[rnd() % 12];
        const char *hp = ports[rnd() % 12];
        memcpy(o + ol, op, strlen(op));
        ol += strlen(op);
        memcpy(h + hl, hp, strlen(hp));
        hl += strlen(hp);
        uint16_t pp = pps[rnd() % 7];
        bool got = http_guard_origin_ok_for_port(o, ol, h, hl, pp);
        bool want = oracle(o, ol, h, hl, pp);
        if (got != want) {
            char msg[300];
            snprintf(msg, sizeof(msg), "mismatch it=%d origin=[%.*s] host=[%.*s] pp=%u got=%d want=%d", it, (int)ol, o, (int)hl, h,
                     (unsigned)pp, got, want);
            TEST_FAIL_MESSAGE(msg);
        }
        if (got) accepted++;
    }
    printf("origin fuzz accepted=%ld\n", accepted);
    TEST_ASSERT_TRUE_MESSAGE(accepted > 500, "fuzz accepted too few inputs (ineffective)");
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_accept_main_port_443);
    RUN_TEST(test_accept_nonstandard_main_port);
    RUN_TEST(test_port_mismatch);
    RUN_TEST(test_host_name_mismatch);
    RUN_TEST(test_case_insensitive);
    RUN_TEST(test_reject_scheme);
    RUN_TEST(test_reject_userinfo_ipv6_trailing_dot_path);
    RUN_TEST(test_reject_bad_ports);
    RUN_TEST(test_reject_empty_and_null_inputs);
    RUN_TEST(test_length_arguments_are_respected);
    RUN_TEST(test_extreme_lengths);
    RUN_TEST(test_name_of_255_with_trailing_dot);
    RUN_TEST(test_consistent_with_existing_origin_ok);
    RUN_TEST(test_fuzz_against_oracle);
    return UNITY_END();
}
