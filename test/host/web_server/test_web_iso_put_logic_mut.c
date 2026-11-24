#include "unity.h"
#include "web_iso_put_logic.h"

#include <limits.h>
#include <stdint.h>
#include <string.h>

/*
 * web_iso_put_logic の追加のテスト(C-16。ミューテーションテストの生き残りを殺すためのもの。docs/ISO_PUT_MSC_TESTS.md)。
 * test_web_iso_put_logic.c(A-15 の PR)は書き換えない。部品(モック)は、このファイルの中だけ。
 *
 * モック: 時計は、now_us の呼び出しごとに、決めた値を順に返す(使い切ったら、最後の値を返し続ける)。recv は、決めた戻り値を順に返し、
 * 正のとき(max_len までの)ペイロードを buf に写す。呼び出しの順序(N = now_us、S = set_timeout_us、R = recv)、set_timeout の値、
 * recv に渡された buf の位置と max_len、ctx の取り違えを記録する。決めた数を超えて呼ばれたら、そのテストを失敗にする
 * (ループの終わらないミュータントを、タイムアウトを待たずに殺す)。
 */

#define MAXN 32u
#define GUARD 0xA5u

typedef struct {
    uint64_t clk[MAXN];
    size_t clk_n;
    size_t now_calls;

    int rets[MAXN];
    size_t rets_n;
    size_t recv_calls;
    const uint8_t *payload;
    size_t payload_len;
    size_t payload_off;

    uint64_t slices[MAXN];
    size_t slice_n;

    const uint8_t *recv_buf_base;
    size_t recv_off[MAXN];
    size_t recv_max[MAXN];

    char trace[4u * MAXN];
    size_t trace_n;
    unsigned bad_ctx;
    size_t total_calls;
} mock_t;

static mock_t M;

static void tr(char c) {
    TEST_ASSERT_LESS_THAN_size_t(sizeof(M.trace) - 1u, M.trace_n);
    M.trace[M.trace_n++] = c;
    M.trace[M.trace_n] = '\0';
    M.total_calls++;
    TEST_ASSERT_LESS_THAN_size_t(120u, M.total_calls); /* ループが終わらない */
}

static uint64_t m_now(void *ctx) {
    if (ctx != &M) {
        M.bad_ctx++;
    }
    tr('N');
    const size_t i = M.now_calls++;
    if (M.clk_n == 0u) {
        return 0u;
    }
    return M.clk[(i < M.clk_n) ? i : (M.clk_n - 1u)];
}

static void m_set_timeout(void *ctx, uint64_t t) {
    if (ctx != &M) {
        M.bad_ctx++;
    }
    tr('S');
    TEST_ASSERT_LESS_THAN_size_t(MAXN, M.slice_n);
    M.slices[M.slice_n++] = t;
}

static int m_recv(void *ctx, uint8_t *buf, size_t max_len) {
    if (ctx != &M) {
        M.bad_ctx++;
    }
    tr('R');
    TEST_ASSERT_LESS_THAN_size_t(M.rets_n, M.recv_calls); /* 決めた戻り値の数を超えて呼ばれた */
    const size_t i = M.recv_calls++;
    M.recv_off[i] = (size_t)(buf - M.recv_buf_base);
    M.recv_max[i] = max_len;
    const int r = M.rets[i];
    if (r > 0 && (size_t)r <= max_len && M.payload_off + (size_t)r <= M.payload_len) {
        memcpy(buf, M.payload + M.payload_off, (size_t)r);
        M.payload_off += (size_t)r;
    }
    return r;
}

static web_iso_put_io_t io_full(void) {
    web_iso_put_io_t io = {.ctx = &M, .now_us = m_now, .set_timeout_us = m_set_timeout, .recv = m_recv};
    return io;
}

static void clk(size_t n, const uint64_t *v) {
    M.clk_n = n;
    memcpy(M.clk, v, n * sizeof(v[0]));
}

static void rets(size_t n, const int *v) {
    M.rets_n = n;
    memcpy(M.rets, v, n * sizeof(v[0]));
}

void setUp(void) { memset(&M, 0, sizeof(M)); }
void tearDown(void) {}

/* ctx の取り違えがなかったこと(全てのコールバックが、io.ctx をそのまま受け取った) */
static void check_ctx(void) { TEST_ASSERT_EQUAL_UINT(0u, M.bad_ctx); }

/* ---- web_iso_put_recv_all ---- */

void test_invalid_arguments_are_rejected_without_calling_any_callback(void) {
    uint8_t buf[8];
    web_iso_put_io_t ok = io_full();
    const uint64_t c[1] = {0u};
    const int rv[1] = {4};
    clk(1u, c);
    rets(1u, rv);

    web_iso_put_io_t no_now = ok;
    no_now.now_us = NULL;
    web_iso_put_io_t no_recv = ok;
    no_recv.recv = NULL;

    TEST_ASSERT_EQUAL_INT(WEB_ISO_PUT_ERR_INVALID, web_iso_put_recv_all(NULL, buf, 8u, 4u, 1000ULL));
    TEST_ASSERT_EQUAL_INT(WEB_ISO_PUT_ERR_INVALID, web_iso_put_recv_all(&no_now, buf, 8u, 4u, 1000ULL));
    TEST_ASSERT_EQUAL_INT(WEB_ISO_PUT_ERR_INVALID, web_iso_put_recv_all(&no_recv, buf, 8u, 4u, 1000ULL));
    TEST_ASSERT_EQUAL_INT(WEB_ISO_PUT_ERR_INVALID, web_iso_put_recv_all(&ok, NULL, 8u, 4u, 1000ULL));
    TEST_ASSERT_EQUAL_INT(WEB_ISO_PUT_ERR_INVALID, web_iso_put_recv_all(&ok, buf, 8u, 0u, 1000ULL));
    TEST_ASSERT_EQUAL_INT(WEB_ISO_PUT_ERR_INVALID, web_iso_put_recv_all(&ok, buf, 8u, 9u, 1000ULL));
    TEST_ASSERT_EQUAL_INT(WEB_ISO_PUT_ERR_INVALID, web_iso_put_recv_all(&ok, buf, 0u, 1u, 1000ULL));
    TEST_ASSERT_EQUAL_INT(WEB_ISO_PUT_ERR_INVALID, web_iso_put_recv_all(&ok, buf, 8u, 4u, 0ULL));
    TEST_ASSERT_EQUAL_STRING("", M.trace); /* 何も呼ばない */
    TEST_ASSERT_EQUAL_UINT(0u, M.now_calls);
    TEST_ASSERT_EQUAL_UINT(0u, M.recv_calls);
}

void test_boundary_arguments_are_accepted(void) {
    /* want_len == cap、timeout_us == 1 は有効 */
    uint8_t src[4] = {1, 2, 3, 4};
    uint8_t dst[4] = {0};
    const web_iso_put_io_t io = io_full();
    const uint64_t c[2] = {10u, 10u};
    const int rv[1] = {4};
    clk(2u, c);
    rets(1u, rv);
    M.payload = src;
    M.payload_len = sizeof(src);
    M.recv_buf_base = dst;
    TEST_ASSERT_EQUAL_INT(WEB_ISO_PUT_OK, web_iso_put_recv_all(&io, dst, sizeof(dst), sizeof(dst), 1ULL));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(src, dst, 4u);
    TEST_ASSERT_EQUAL_UINT64(WEB_ISO_PUT_MIN_SLICE_US, M.slices[0]); /* 残り 1 us は、最小のスライスまで引き上げる */
    check_ctx();
}

/* 1 回目の確認(ループの先頭)で、経過が elapsed のとき */
static int first_check(uint64_t timeout_us, uint64_t elapsed, int recv_ret) {
    static uint8_t buf[8];
    static const uint8_t src[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    memset(&M, 0, sizeof(M));
    const web_iso_put_io_t io = io_full();
    const uint64_t c[2] = {1000000ULL, 1000000ULL + elapsed};
    const int rv[1] = {recv_ret};
    clk(2u, c);
    rets(1u, rv);
    M.payload = src;
    M.payload_len = sizeof(src);
    M.recv_buf_base = buf;
    return web_iso_put_recv_all(&io, buf, sizeof(buf), 4u, timeout_us);
}

void test_deadline_is_checked_at_the_top_of_the_loop_with_an_exclusive_boundary(void) {
    const uint64_t T = 1000000ULL;
    /* 経過が T - 1: まだ期限の前。recv する(最小のスライス) */
    TEST_ASSERT_EQUAL_INT(WEB_ISO_PUT_OK, first_check(T, T - 1u, 4));
    TEST_ASSERT_EQUAL_STRING("NNSR", M.trace);
    TEST_ASSERT_EQUAL_UINT64(WEB_ISO_PUT_MIN_SLICE_US, M.slices[0]);
    /* 経過が T: 期限。recv も set_timeout もしない */
    TEST_ASSERT_EQUAL_INT(WEB_ISO_PUT_ERR_TIMEOUT, first_check(T, T, 4));
    TEST_ASSERT_EQUAL_STRING("NN", M.trace);
    TEST_ASSERT_EQUAL_INT(WEB_ISO_PUT_ERR_TIMEOUT, first_check(T, T + 1u, 4));
    TEST_ASSERT_EQUAL_STRING("NN", M.trace);
    /* 経過 0 */
    TEST_ASSERT_EQUAL_INT(WEB_ISO_PUT_OK, first_check(T, 0u, 4));
    TEST_ASSERT_EQUAL_UINT64(T, M.slices[0]);
    check_ctx();
}

void test_slice_is_the_remaining_time_but_never_below_the_minimum(void) {
    const uint64_t T = 5000000ULL;
    const uint64_t MIN = WEB_ISO_PUT_MIN_SLICE_US;
    /* 残り = MIN - 1 → MIN、MIN → MIN、MIN + 1 → MIN + 1、それ以上は残りそのまま */
    TEST_ASSERT_EQUAL_INT(WEB_ISO_PUT_OK, first_check(T, T - (MIN - 1u), 4));
    TEST_ASSERT_EQUAL_UINT64(MIN, M.slices[0]);
    TEST_ASSERT_EQUAL_INT(WEB_ISO_PUT_OK, first_check(T, T - MIN, 4));
    TEST_ASSERT_EQUAL_UINT64(MIN, M.slices[0]);
    TEST_ASSERT_EQUAL_INT(WEB_ISO_PUT_OK, first_check(T, T - (MIN + 1u), 4));
    TEST_ASSERT_EQUAL_UINT64(MIN + 1u, M.slices[0]);
    TEST_ASSERT_EQUAL_INT(WEB_ISO_PUT_OK, first_check(T, T - 3u * MIN, 4));
    TEST_ASSERT_EQUAL_UINT64(3u * MIN, M.slices[0]);
    TEST_ASSERT_EQUAL_INT(WEB_ISO_PUT_OK, first_check(T, 1234567ULL, 4));
    TEST_ASSERT_EQUAL_UINT64(T - 1234567ULL, M.slices[0]);
    /* 非常に大きい期限でも、そのまま */
    TEST_ASSERT_EQUAL_INT(WEB_ISO_PUT_OK, first_check(UINT64_MAX, 0u, 4));
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, M.slices[0]);
    check_ctx();
}

void test_set_timeout_runs_before_every_recv_and_is_optional(void) {
    uint8_t src[6] = {1, 2, 3, 4, 5, 6};
    uint8_t dst[6] = {0};
    web_iso_put_io_t io = io_full();
    const uint64_t c[1] = {0u};
    const int rv[3] = {1, 2, 3};
    clk(1u, c);
    rets(3u, rv);
    M.payload = src;
    M.payload_len = sizeof(src);
    M.recv_buf_base = dst;
    TEST_ASSERT_EQUAL_INT(WEB_ISO_PUT_OK, web_iso_put_recv_all(&io, dst, sizeof(dst), sizeof(dst), 1000000ULL));
    /* start の読み、その後、周回ごとに N S R */
    TEST_ASSERT_EQUAL_STRING("NNSRNSRNSR", M.trace);
    TEST_ASSERT_EQUAL_UINT(3u, M.slice_n);
    check_ctx();

    /* set_timeout_us が NULL でも動く(S なし) */
    memset(&M, 0, sizeof(M));
    memset(dst, 0, sizeof(dst));
    io.set_timeout_us = NULL;
    clk(1u, c);
    rets(3u, rv);
    M.payload = src;
    M.payload_len = sizeof(src);
    M.recv_buf_base = dst;
    TEST_ASSERT_EQUAL_INT(WEB_ISO_PUT_OK, web_iso_put_recv_all(&io, dst, sizeof(dst), sizeof(dst), 1000000ULL));
    TEST_ASSERT_EQUAL_STRING("NNRNRNR", M.trace);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(src, dst, sizeof(src));
}

void test_recv_gets_the_next_position_and_the_remaining_length_and_never_overruns(void) {
    uint8_t src[10] = {10, 11, 12, 13, 14, 15, 16, 17, 18, 19};
    uint8_t dst[16];
    memset(dst, GUARD, sizeof(dst));
    const web_iso_put_io_t io = io_full();
    const uint64_t c[1] = {0u};
    const int rv[3] = {3, 4, 3};
    clk(1u, c);
    rets(3u, rv);
    M.payload = src;
    M.payload_len = sizeof(src);
    M.recv_buf_base = dst;
    /* cap(16)より小さい want_len(10): 受け取るのは 10 バイトだけ */
    TEST_ASSERT_EQUAL_INT(WEB_ISO_PUT_OK, web_iso_put_recv_all(&io, dst, sizeof(dst), 10u, 1000000ULL));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(src, dst, 10u);
    for (size_t i = 10u; i < sizeof(dst); i++) {
        TEST_ASSERT_EQUAL_HEX8(GUARD, dst[i]);
    }
    TEST_ASSERT_EQUAL_UINT(3u, M.recv_calls);
    TEST_ASSERT_EQUAL_UINT(0u, M.recv_off[0]);
    TEST_ASSERT_EQUAL_UINT(3u, M.recv_off[1]);
    TEST_ASSERT_EQUAL_UINT(7u, M.recv_off[2]);
    TEST_ASSERT_EQUAL_UINT(10u, M.recv_max[0]);
    TEST_ASSERT_EQUAL_UINT(7u, M.recv_max[1]);
    TEST_ASSERT_EQUAL_UINT(3u, M.recv_max[2]);
    check_ctx();
}

void test_a_read_longer_than_requested_is_an_io_error_but_exactly_the_rest_is_fine(void) {
    uint8_t dst[8];
    const web_iso_put_io_t io = io_full();
    const uint64_t c[1] = {0u};

    /* 1 回目から多い(rem + 1) */
    const int over1[1] = {5};
    clk(1u, c);
    rets(1u, over1);
    M.recv_buf_base = dst;
    TEST_ASSERT_EQUAL_INT(WEB_ISO_PUT_ERR_IO, web_iso_put_recv_all(&io, dst, sizeof(dst), 4u, 1000000ULL));
    TEST_ASSERT_EQUAL_UINT(1u, M.recv_calls);

    /* 途中から多い: 2 + 5(残りは 4) */
    memset(&M, 0, sizeof(M));
    const int over2[2] = {2, 5};
    clk(1u, c);
    rets(2u, over2);
    M.recv_buf_base = dst;
    TEST_ASSERT_EQUAL_INT(WEB_ISO_PUT_ERR_IO, web_iso_put_recv_all(&io, dst, sizeof(dst), 6u, 1000000ULL));
    TEST_ASSERT_EQUAL_UINT(2u, M.recv_calls);

    /* 残りちょうどは OK(2 + 4) */
    memset(&M, 0, sizeof(M));
    const int exact[2] = {2, 4};
    clk(1u, c);
    rets(2u, exact);
    M.recv_buf_base = dst;
    TEST_ASSERT_EQUAL_INT(WEB_ISO_PUT_OK, web_iso_put_recv_all(&io, dst, sizeof(dst), 6u, 1000000ULL));
    TEST_ASSERT_EQUAL_UINT(2u, M.recv_calls);
    check_ctx();
}

void test_every_non_positive_recv_result_other_than_the_timeout_sentinel_is_an_io_error(void) {
    uint8_t dst[8];
    const web_iso_put_io_t io = io_full();
    const uint64_t c[1] = {0u};
    const int codes[] = {0, WEB_ISO_PUT_IO_ERROR, WEB_ISO_PUT_IO_TIMEOUT - 1, -4, -100, INT_MIN};
    for (size_t k = 0; k < sizeof(codes) / sizeof(codes[0]); k++) {
        /* 最初の recv で */
        memset(&M, 0, sizeof(M));
        const int a[1] = {codes[k]};
        clk(1u, c);
        rets(1u, a);
        M.recv_buf_base = dst;
        TEST_ASSERT_EQUAL_INT(WEB_ISO_PUT_ERR_IO, web_iso_put_recv_all(&io, dst, sizeof(dst), 4u, 1000000ULL));
        TEST_ASSERT_EQUAL_UINT(1u, M.recv_calls);
        /* データを受け取った後で */
        memset(&M, 0, sizeof(M));
        const int b[2] = {2, codes[k]};
        clk(1u, c);
        rets(2u, b);
        M.recv_buf_base = dst;
        TEST_ASSERT_EQUAL_INT(WEB_ISO_PUT_ERR_IO, web_iso_put_recv_all(&io, dst, sizeof(dst), 4u, 1000000ULL));
        TEST_ASSERT_EQUAL_UINT(2u, M.recv_calls);
    }
    check_ctx();
}

void test_a_recv_timeout_retries_until_the_total_deadline_and_the_slice_shrinks(void) {
    uint8_t src[4] = {9, 8, 7, 6};
    uint8_t dst[4] = {0};
    const web_iso_put_io_t io = io_full();
    const uint64_t T = 1000000ULL;
    /* 読み: start=0、top1=0、now2=300000、top2=300000、now2=600000、top3=600000、(データ) */
    const uint64_t c[7] = {0u, 0u, 300000u, 300000u, 600000u, 600000u, 600000u};
    const int rv[3] = {WEB_ISO_PUT_IO_TIMEOUT, WEB_ISO_PUT_IO_TIMEOUT, 4};
    clk(7u, c);
    rets(3u, rv);
    M.payload = src;
    M.payload_len = sizeof(src);
    M.recv_buf_base = dst;
    TEST_ASSERT_EQUAL_INT(WEB_ISO_PUT_OK, web_iso_put_recv_all(&io, dst, sizeof(dst), 4u, T));
    TEST_ASSERT_EQUAL_STRING("NNSRNNSRNNSR", M.trace);
    TEST_ASSERT_EQUAL_UINT(3u, M.slice_n);
    TEST_ASSERT_EQUAL_UINT64(T, M.slices[0]);
    TEST_ASSERT_EQUAL_UINT64(T - 300000ULL, M.slices[1]);
    TEST_ASSERT_EQUAL_UINT64(T - 600000ULL, M.slices[2]);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(src, dst, 4u);
    check_ctx();
}

void test_a_recv_timeout_ends_at_once_when_the_deadline_has_passed_on_a_fresh_clock_read(void) {
    uint8_t dst[4] = {0};
    const web_iso_put_io_t io = io_full();
    const uint64_t T = 1000000ULL;
    /* now2 - start == T - 1 : まだ。続ける。ループの先頭で、もう 1 回時計を読む */
    {
        const uint64_t c[5] = {100u, 100u, 100u + T - 1u, 100u + T - 1u, 100u + T - 1u};
        const int rv[2] = {WEB_ISO_PUT_IO_TIMEOUT, 4};
        memset(&M, 0, sizeof(M));
        clk(5u, c);
        rets(2u, rv);
        const uint8_t src[4] = {1, 2, 3, 4};
        M.payload = src;
        M.payload_len = 4u;
        M.recv_buf_base = dst;
        TEST_ASSERT_EQUAL_INT(WEB_ISO_PUT_OK, web_iso_put_recv_all(&io, dst, sizeof(dst), 4u, T));
        TEST_ASSERT_EQUAL_STRING("NNSRNNSR", M.trace);
        TEST_ASSERT_EQUAL_UINT64(WEB_ISO_PUT_MIN_SLICE_US, M.slices[1]);
    }
    /* now2 - start == T : 期限。直ちに TIMEOUT(次の周回の時計の読みと recv がない。後の読みは、期限の前を返す時計) */
    {
        const uint64_t c[4] = {100u, 100u, 100u + T, 100u};
        const int rv[2] = {WEB_ISO_PUT_IO_TIMEOUT, 4};
        memset(&M, 0, sizeof(M));
        clk(4u, c);
        rets(2u, rv);
        M.recv_buf_base = dst;
        TEST_ASSERT_EQUAL_INT(WEB_ISO_PUT_ERR_TIMEOUT, web_iso_put_recv_all(&io, dst, sizeof(dst), 4u, T));
        TEST_ASSERT_EQUAL_STRING("NNSRN", M.trace);
    }
    /* now2 が start より前(時計が戻った): 期限が非常に大きくても、TIMEOUT(引き算が回り込むのに頼らない) */
    {
        const uint64_t c[4] = {1000u, 1000u, 500u, 1000u};
        const int rv[2] = {WEB_ISO_PUT_IO_TIMEOUT, 4};
        memset(&M, 0, sizeof(M));
        clk(4u, c);
        rets(2u, rv);
        M.recv_buf_base = dst;
        TEST_ASSERT_EQUAL_INT(WEB_ISO_PUT_ERR_TIMEOUT, web_iso_put_recv_all(&io, dst, sizeof(dst), 4u, UINT64_MAX));
        TEST_ASSERT_EQUAL_STRING("NNSRN", M.trace);
    }
    check_ctx();
}

void test_a_recv_timeout_with_a_clock_that_has_not_moved_keeps_waiting(void) {
    uint8_t src[4] = {5, 6, 7, 8};
    uint8_t dst[4] = {0};
    const web_iso_put_io_t io = io_full();
    /* start = top1 = now2 = top2 = 100: 時計が進んでいなくても(経過 0)、期限ではない。もう 1 回 recv する */
    const uint64_t c[1] = {100u};
    const int rv[3] = {WEB_ISO_PUT_IO_TIMEOUT, WEB_ISO_PUT_IO_TIMEOUT, 4};
    clk(1u, c);
    rets(3u, rv);
    M.payload = src;
    M.payload_len = sizeof(src);
    M.recv_buf_base = dst;
    TEST_ASSERT_EQUAL_INT(WEB_ISO_PUT_OK, web_iso_put_recv_all(&io, dst, sizeof(dst), 4u, 1000000ULL));
    TEST_ASSERT_EQUAL_STRING("NNSRNNSRNNSR", M.trace);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(src, dst, 4u);
    check_ctx();
}

void test_a_clock_that_goes_backwards_at_the_top_of_the_loop_is_a_timeout(void) {
    uint8_t dst[4] = {0};
    const web_iso_put_io_t io = io_full();
    /* 100 us 戻る。期限が UINT64_MAX でも、引き算が回り込んで「経過が大きい」ことに頼らず、戻りを検出する */
    const uint64_t c[2] = {1000u, 900u};
    const uint64_t timeouts[] = {1000000ULL, UINT64_MAX};
    for (size_t k = 0; k < 2u; k++) {
        memset(&M, 0, sizeof(M));
        const int rv[1] = {4};
        clk(2u, c);
        rets(1u, rv);
        M.recv_buf_base = dst;
        TEST_ASSERT_EQUAL_INT(WEB_ISO_PUT_ERR_TIMEOUT, web_iso_put_recv_all(&io, dst, sizeof(dst), 4u, timeouts[k]));
        TEST_ASSERT_EQUAL_STRING("NN", M.trace); /* recv も set_timeout もしない */
    }
    check_ctx();
}

/* ---- web_iso_progress_step_pct ---- */

void test_progress_rejects_a_zero_total_and_a_null_bucket_without_touching_the_bucket(void) {
    int last = 7;
    TEST_ASSERT_EQUAL_INT(-1, web_iso_progress_step_pct(0u, 0u, &last));
    TEST_ASSERT_EQUAL_INT(-1, web_iso_progress_step_pct(100u, 0u, &last));
    TEST_ASSERT_EQUAL_INT(7, last);
    last = -1;
    TEST_ASSERT_EQUAL_INT(-1, web_iso_progress_step_pct(UINT64_MAX, 0u, &last));
    TEST_ASSERT_EQUAL_INT(-1, last);
    TEST_ASSERT_EQUAL_INT(-1, web_iso_progress_step_pct(50u, 100u, NULL));
    TEST_ASSERT_EQUAL_INT(-1, web_iso_progress_step_pct(100u, 100u, NULL));
}

void test_progress_matches_an_integer_model_for_every_byte_of_small_totals(void) {
    const uint64_t totals[] = {1u, 3u, 7u, 19u, 20u, 21u, 99u, 100u, 101u, 999u, 1000u};
    for (size_t t = 0; t < sizeof(totals) / sizeof(totals[0]); t++) {
        const uint64_t total = totals[t];
        int last = -1;
        int model_last = -1;
        for (uint64_t rcv = 0; rcv <= total + 2u; rcv++) {
            const uint64_t eff = (rcv > total) ? total : rcv;
            const int model_bucket = (int)((eff * 100u / total) / 5u);
            int expect = -1;
            if (model_bucket > 0 && model_bucket > model_last) {
                model_last = model_bucket;
                expect = model_bucket * 5;
            }
            TEST_ASSERT_EQUAL_INT(expect, web_iso_progress_step_pct(rcv, total, &last));
            TEST_ASSERT_EQUAL_INT(model_last, last);
        }
        TEST_ASSERT_EQUAL_INT(20, last); /* 最後は 100% */
    }
}

void test_progress_suppression_leaves_the_bucket_alone_and_only_a_new_bucket_updates_it(void) {
    int last = -1;
    TEST_ASSERT_EQUAL_INT(-1, web_iso_progress_step_pct(0u, 100u, &last)); /* 0%: バケット 0 は出さない */
    TEST_ASSERT_EQUAL_INT(-1, last);
    TEST_ASSERT_EQUAL_INT(-1, web_iso_progress_step_pct(4u, 100u, &last));
    TEST_ASSERT_EQUAL_INT(-1, last);
    TEST_ASSERT_EQUAL_INT(5, web_iso_progress_step_pct(5u, 100u, &last));
    TEST_ASSERT_EQUAL_INT(1, last);
    TEST_ASSERT_EQUAL_INT(-1, web_iso_progress_step_pct(5u, 100u, &last)); /* 同じバケット */
    TEST_ASSERT_EQUAL_INT(-1, web_iso_progress_step_pct(9u, 100u, &last));
    TEST_ASSERT_EQUAL_INT(1, last);
    TEST_ASSERT_EQUAL_INT(10, web_iso_progress_step_pct(10u, 100u, &last));
    TEST_ASSERT_EQUAL_INT(2, last);
    TEST_ASSERT_EQUAL_INT(-1, web_iso_progress_step_pct(7u, 100u, &last)); /* 戻った */
    TEST_ASSERT_EQUAL_INT(2, last);
    TEST_ASSERT_EQUAL_INT(95, web_iso_progress_step_pct(99u, 100u, &last)); /* 飛び越し: 途中のバケットは出さない */
    TEST_ASSERT_EQUAL_INT(19, last);
    TEST_ASSERT_EQUAL_INT(100, web_iso_progress_step_pct(100u, 100u, &last));
    TEST_ASSERT_EQUAL_INT(20, last);
    TEST_ASSERT_EQUAL_INT(-1, web_iso_progress_step_pct(100u, 100u, &last));
    TEST_ASSERT_EQUAL_INT(-1, web_iso_progress_step_pct(101u, 100u, &last));
    TEST_ASSERT_EQUAL_INT(20, last);

    /* 最初の last_bucket の値が、結果を決める */
    last = 20;
    TEST_ASSERT_EQUAL_INT(-1, web_iso_progress_step_pct(100u, 100u, &last));
    TEST_ASSERT_EQUAL_INT(20, last);
    last = 0;
    TEST_ASSERT_EQUAL_INT(5, web_iso_progress_step_pct(5u, 100u, &last));
    TEST_ASSERT_EQUAL_INT(1, last);
    last = -100;
    TEST_ASSERT_EQUAL_INT(-1, web_iso_progress_step_pct(3u, 100u, &last)); /* バケット 0 は、last が負でも出さない */
    TEST_ASSERT_EQUAL_INT(-100, last);
    last = 1;
    TEST_ASSERT_EQUAL_INT(-1, web_iso_progress_step_pct(5u, 100u, &last)); /* 同じバケット */
    TEST_ASSERT_EQUAL_INT(1, last);
    TEST_ASSERT_EQUAL_INT(-1, web_iso_progress_step_pct(5u, 100u, &last));
}

void test_progress_received_beyond_the_total_is_one_hundred_percent(void) {
    int last = -1;
    TEST_ASSERT_EQUAL_INT(100, web_iso_progress_step_pct(1001u, 1000u, &last));
    TEST_ASSERT_EQUAL_INT(20, last);
    last = -1;
    TEST_ASSERT_EQUAL_INT(100, web_iso_progress_step_pct(UINT64_MAX, 1u, &last));
    last = -1;
    TEST_ASSERT_EQUAL_INT(100, web_iso_progress_step_pct(UINT64_MAX, UINT64_MAX, &last));
    last = -1;
    TEST_ASSERT_EQUAL_INT(100, web_iso_progress_step_pct(1u, 1u, &last));
}

void test_progress_switches_to_the_division_first_path_exactly_above_uint64_max_div_100(void) {
    const uint64_t R = UINT64_MAX / 100ULL; /* これ以下は、received * 100 が回り込まないので、正確な計算 */
    int last = -1;

    /* received == R: 正確な計算。真の割合は 5% に少し足りない(4.99..%)ので、-1。
     * (割り算を先にする近似は、切り捨ての分だけ大きく出て 5 になる) */
    TEST_ASSERT_EQUAL_INT(-1, web_iso_progress_step_pct(R, 20ULL * R + 15ULL, &last));
    TEST_ASSERT_EQUAL_INT(-1, last);
    /* 同じ割合で、received が R + 1: 近似の計算。結果は、近似の分だけ 5 になる */
    last = -1;
    TEST_ASSERT_EQUAL_INT(5, web_iso_progress_step_pct(R + 1ULL, 20ULL * (R + 1ULL) + 15ULL, &last));
    TEST_ASSERT_EQUAL_INT(1, last);

    /* received == R + 1、割合 50%(received * 100 は回り込むので、近似の計算でなければ誤る) */
    last = -1;
    TEST_ASSERT_EQUAL_INT(50, web_iso_progress_step_pct(R + 1ULL, 2ULL * (R + 1ULL), &last));
    TEST_ASSERT_EQUAL_INT(10, last);

    /* R のすぐ上と下の received で、割合 50%(正確な計算の側) */
    last = -1;
    TEST_ASSERT_EQUAL_INT(50, web_iso_progress_step_pct(R, 2ULL * R, &last));
    TEST_ASSERT_EQUAL_INT(10, last);
}

void test_progress_large_totals_use_the_division_first_path_and_stay_in_range(void) {
    const uint64_t d = 100000000000000000ULL; /* 1e17: total = 100 * d = 1e19(UINT64_MAX / 100 より大きい) */
    const uint64_t total = 100ULL * d;
    int last = -1;
    TEST_ASSERT_EQUAL_INT(-1, web_iso_progress_step_pct(0u, total, &last));
    TEST_ASSERT_EQUAL_INT(-1, web_iso_progress_step_pct(d, total, &last));         /* 1% */
    TEST_ASSERT_EQUAL_INT(-1, web_iso_progress_step_pct(5ULL * d - 1ULL, total, &last)); /* 4.99..% */
    TEST_ASSERT_EQUAL_INT(5, web_iso_progress_step_pct(5ULL * d, total, &last));
    TEST_ASSERT_EQUAL_INT(1, last);
    TEST_ASSERT_EQUAL_INT(-1, web_iso_progress_step_pct(5ULL * d + 1ULL, total, &last));
    TEST_ASSERT_EQUAL_INT(10, web_iso_progress_step_pct(10ULL * d, total, &last));
    TEST_ASSERT_EQUAL_INT(50, web_iso_progress_step_pct(total / 2ULL, total, &last));
    TEST_ASSERT_EQUAL_INT(95, web_iso_progress_step_pct(total - 1ULL, total, &last)); /* 99.99..%: 95 */
    TEST_ASSERT_EQUAL_INT(19, last);
    TEST_ASSERT_EQUAL_INT(100, web_iso_progress_step_pct(total, total, &last));
    TEST_ASSERT_EQUAL_INT(20, last);
    TEST_ASSERT_EQUAL_INT(-1, web_iso_progress_step_pct(total, total, &last));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_invalid_arguments_are_rejected_without_calling_any_callback);
    RUN_TEST(test_boundary_arguments_are_accepted);
    RUN_TEST(test_deadline_is_checked_at_the_top_of_the_loop_with_an_exclusive_boundary);
    RUN_TEST(test_slice_is_the_remaining_time_but_never_below_the_minimum);
    RUN_TEST(test_set_timeout_runs_before_every_recv_and_is_optional);
    RUN_TEST(test_recv_gets_the_next_position_and_the_remaining_length_and_never_overruns);
    RUN_TEST(test_a_read_longer_than_requested_is_an_io_error_but_exactly_the_rest_is_fine);
    RUN_TEST(test_every_non_positive_recv_result_other_than_the_timeout_sentinel_is_an_io_error);
    RUN_TEST(test_a_recv_timeout_retries_until_the_total_deadline_and_the_slice_shrinks);
    RUN_TEST(test_a_recv_timeout_ends_at_once_when_the_deadline_has_passed_on_a_fresh_clock_read);
    RUN_TEST(test_a_recv_timeout_with_a_clock_that_has_not_moved_keeps_waiting);
    RUN_TEST(test_a_clock_that_goes_backwards_at_the_top_of_the_loop_is_a_timeout);
    RUN_TEST(test_progress_rejects_a_zero_total_and_a_null_bucket_without_touching_the_bucket);
    RUN_TEST(test_progress_matches_an_integer_model_for_every_byte_of_small_totals);
    RUN_TEST(test_progress_suppression_leaves_the_bucket_alone_and_only_a_new_bucket_updates_it);
    RUN_TEST(test_progress_received_beyond_the_total_is_one_hundred_percent);
    RUN_TEST(test_progress_switches_to_the_division_first_path_exactly_above_uint64_max_div_100);
    RUN_TEST(test_progress_large_totals_use_the_division_first_path_and_stay_in_range);
    return UNITY_END();
}
