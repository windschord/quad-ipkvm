/*
 * http_guard_video_accept のファズ (docs/VIDEO_TRANSPORT.md 9.4。P7)。
 * 固定シードの疑似乱数で、クエリに出やすい文字 (accept の綴り、h264、= & , % NUL など) の短い列を作り、
 *  (1) 落ちない・範囲外を読まない (入力は、長さちょうどのヒープ領域に置く。ASan / UBSan)、
 *  (2) 別の書き方のオラクル (NUL 終端の文字列にして、strchr / strcmp で分ける) と同じ結果になる、
 * ことを確かめる。ランダムなバイト列 (全 256 値) でも、同じ。
 */
#include "unity.h"
#include "http_guard.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

void setUp(void) {}
void tearDown(void) {}

static uint32_t rng_state = 1;
static uint32_t rnd(void) {
    uint32_t x = rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rng_state = x;
    return x;
}

/* オラクル: q の len バイトを NUL 終端のコピーにして、& で分け、最初の = で key / value に分け、
 * key == "accept" のとき value を , で分け、"h264" があれば真。実装とは別の書き方 (strchr / strcmp)。
 * 長さの上限を超える、または NUL を含む入力は 0 (仕様)。 */
static uint32_t oracle(const char *q, size_t len) {
    if (len == 0 || len > HTTP_GUARD_VIDEO_QUERY_MAX) return 0;
    if (memchr(q, 0, len) != NULL) return 0;
    char *copy = (char *)malloc(len + 1);
    memcpy(copy, q, len);
    copy[len] = '\0';
    uint32_t mask = 0;
    char *seg = copy;
    for (;;) {
        char *amp = strchr(seg, '&');
        if (amp) *amp = '\0';
        char *eq = strchr(seg, '=');
        if (eq) {
            *eq = '\0';
            if (strcmp(seg, "accept") == 0) {
                char *val = eq + 1;
                for (;;) {
                    char *comma = strchr(val, ',');
                    if (comma) *comma = '\0';
                    if (strcmp(val, "h264") == 0) mask |= 1u;
                    if (!comma) break;
                    val = comma + 1;
                }
            }
        }
        if (!amp) break;
        seg = amp + 1;
    }
    free(copy);
    return mask;
}

static const char *const TOKENS[] = {
    "accept", "accept=", "h264", "H264", "h265", ",", "&", "=", "%", "%68", "xaccept", "accept2", "Accept",
    "x", "a", "1", " ", "+", "?", "av1", "h26", "264", "\xff", "accept=h264", "&accept=h264", "accept=h264,",
};

static size_t gen(char *buf, size_t max, int with_nul) {
    size_t n = 0;
    size_t ntok = rnd() % 12;
    for (size_t i = 0; i < ntok; i++) {
        const char *t = TOKENS[rnd() % (sizeof(TOKENS) / sizeof(TOKENS[0]))];
        size_t tl = strlen(t);
        if (n + tl > max) break;
        memcpy(buf + n, t, tl);
        n += tl;
        if (with_nul && (rnd() % 40) == 0 && n < max) buf[n++] = '\0';
    }
    return n;
}

void test_fuzz_matches_oracle(void) {
    rng_state = 0x12345u;
    char tmp[1024];
    uint32_t hits = 0, zeros = 0;
    for (int i = 0; i < 200000; i++) {
        size_t n = gen(tmp, 200, 0);
        char *p = (char *)malloc(n ? n : 1); /* ちょうどの長さ。NUL なし */
        if (n) memcpy(p, tmp, n);
        uint32_t got = http_guard_video_accept(p, n);
        uint32_t want = oracle(p, n);
        if (got != want) {
            char msg[400];
            snprintf(msg, sizeof(msg), "iter %d len %zu query=[%.*s]", i, n, (int)n, p);
            free(p);
            TEST_FAIL_MESSAGE(msg);
        }
        if (got) hits++; else zeros++;
        free(p);
    }
    /* 生成が偏っていない (両方の結果が十分に出る) ことの確認 */
    TEST_ASSERT_TRUE_MESSAGE(hits > 5000, "too few h264 cases");
    TEST_ASSERT_TRUE_MESSAGE(zeros > 5000, "too few non-h264 cases");
}

void test_fuzz_with_nul_matches_oracle(void) {
    rng_state = 0xBEEFu;
    char tmp[1024];
    for (int i = 0; i < 100000; i++) {
        size_t n = gen(tmp, 200, 1);
        char *p = (char *)malloc(n ? n : 1);
        if (n) memcpy(p, tmp, n);
        TEST_ASSERT_EQUAL_UINT32(oracle(p, n), http_guard_video_accept(p, n));
        free(p);
    }
}

void test_fuzz_random_bytes(void) {
    rng_state = 0xC0FFEEu;
    for (int i = 0; i < 60000; i++) {
        size_t n = rnd() % 700; /* 上限 512 の前後を含む */
        char *p = (char *)malloc(n ? n : 1);
        for (size_t k = 0; k < n; k++) p[k] = (char)(rnd() & 0xFF);
        uint32_t got = http_guard_video_accept(p, n);
        TEST_ASSERT_EQUAL_UINT32(got & HTTP_GUARD_VIDEO_ACCEPT_H264, got);
        TEST_ASSERT_EQUAL_UINT32(oracle(p, n), got);
        free(p);
    }
}

void test_fuzz_prefixes_of_valid_query(void) {
    /* 有効なクエリの、すべての長さの接頭辞 (切ったとき) を、ちょうどの領域に置いて呼ぶ。範囲外を読まない */
    static const char *const qs[] = {"accept=h264", "foo=1&accept=h264,h265&bar=2", "accept=h265&accept=h264",
                                     "xaccept=h264&accept="};
    for (size_t k = 0; k < sizeof(qs) / sizeof(qs[0]); k++) {
        size_t full = strlen(qs[k]);
        for (size_t n = 0; n <= full; n++) {
            char *p = (char *)malloc(n ? n : 1);
            if (n) memcpy(p, qs[k], n);
            TEST_ASSERT_EQUAL_UINT32(oracle(p, n), http_guard_video_accept(p, n));
            free(p);
        }
    }
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_fuzz_matches_oracle);
    RUN_TEST(test_fuzz_with_nul_matches_oracle);
    RUN_TEST(test_fuzz_random_bytes);
    RUN_TEST(test_fuzz_prefixes_of_valid_query);
    return UNITY_END();
}
