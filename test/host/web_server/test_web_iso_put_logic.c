#include "unity.h"
#include "web_iso_put_logic.h"

#include <stdint.h>
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

typedef struct {
    const uint8_t *payload;
    size_t payload_len;
    size_t payload_off;

    int script_ret[16];
    uint64_t script_dt_us[16];
    size_t script_len;
    size_t script_idx;

    uint64_t now;
    uint64_t timeout_history[16];
    size_t timeout_calls;
} mock_put_io_t;

static uint64_t mock_now_us(void *ctx) {
    const mock_put_io_t *m = (const mock_put_io_t *)ctx;
    return m->now;
}

static void mock_set_timeout_us(void *ctx, uint64_t timeout_us) {
    mock_put_io_t *m = (mock_put_io_t *)ctx;
    if (m->timeout_calls < sizeof(m->timeout_history) / sizeof(m->timeout_history[0])) {
        m->timeout_history[m->timeout_calls++] = timeout_us;
    }
}

static int mock_recv(void *ctx, uint8_t *buf, size_t max_len) {
    mock_put_io_t *m = (mock_put_io_t *)ctx;
    if (m->script_idx < m->script_len) {
        const size_t i = m->script_idx++;
        m->now += m->script_dt_us[i];
        const int r = m->script_ret[i];
        if (r > 0) {
            const size_t n = (size_t)r;
            if (n <= max_len && m->payload_off + n <= m->payload_len) {
                memcpy(buf, m->payload + m->payload_off, n);
                m->payload_off += n;
            }
        }
        return r;
    }
    return 0;
}

static web_iso_put_io_t make_io(mock_put_io_t *m) {
    return (web_iso_put_io_t){
        .ctx = m,
        .now_us = mock_now_us,
        .set_timeout_us = mock_set_timeout_us,
        .recv = mock_recv,
    };
}

void test_recv_all_single_chunk_exact(void) {
    const uint8_t src[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    uint8_t dst[8] = {0};
    mock_put_io_t m = {
        .payload = src,
        .payload_len = sizeof(src),
        .script_ret = {8},
        .script_dt_us = {5000ULL},
        .script_len = 1u,
        .now = 100000ULL,
    };
    const web_iso_put_io_t io = make_io(&m);
    TEST_ASSERT_EQUAL_INT(WEB_ISO_PUT_OK,
                          web_iso_put_recv_all(&io, dst, sizeof(dst), sizeof(src), 2000000ULL));
    TEST_ASSERT_EQUAL_MEMORY(src, dst, sizeof(src));
    TEST_ASSERT_EQUAL_UINT(1u, m.timeout_calls);
    TEST_ASSERT_EQUAL_UINT64(2000000ULL, m.timeout_history[0]);
}

void test_recv_all_partial_reads_and_min_slice_clamp(void) {
    const uint8_t src[6] = {'A', 'B', 'C', 'D', 'E', 'F'};
    uint8_t dst[6] = {0};
    mock_put_io_t m = {
        .payload = src,
        .payload_len = sizeof(src),
        .script_ret = {2, WEB_ISO_PUT_IO_TIMEOUT, 3, 1},
        .script_dt_us = {500000ULL, 1495000ULL, 1000ULL, 500ULL},
        .script_len = 4u,
        .now = 1000000ULL,
    };
    const web_iso_put_io_t io = make_io(&m);
    TEST_ASSERT_EQUAL_INT(WEB_ISO_PUT_OK,
                          web_iso_put_recv_all(&io, dst, sizeof(dst), sizeof(src), 2000000ULL));
    TEST_ASSERT_EQUAL_MEMORY(src, dst, sizeof(src));
    TEST_ASSERT_EQUAL_UINT(4u, m.timeout_calls);
    TEST_ASSERT_EQUAL_UINT64(2000000ULL, m.timeout_history[0]);
    TEST_ASSERT_EQUAL_UINT64(1500000ULL, m.timeout_history[1]);
    /* Only 5000 us remained (< WEB_ISO_PUT_MIN_SLICE_US), so slice must clamp to 10000 us. */
    TEST_ASSERT_EQUAL_UINT64(WEB_ISO_PUT_MIN_SLICE_US, m.timeout_history[2]);
    TEST_ASSERT_EQUAL_UINT64(WEB_ISO_PUT_MIN_SLICE_US, m.timeout_history[3]);
}

void test_recv_all_timeout_before_and_after_recv(void) {
    const uint8_t src[6] = {10, 20, 30, 40, 50, 60};
    uint8_t dst[6] = {0};

    /* Partial read exhausts total deadline before next recv call. */
    mock_put_io_t m1 = {
        .payload = src,
        .payload_len = sizeof(src),
        .script_ret = {2, 4},
        .script_dt_us = {2000000ULL, 1000ULL},
        .script_len = 2u,
        .now = 500ULL,
    };
    const web_iso_put_io_t io1 = make_io(&m1);
    TEST_ASSERT_EQUAL_INT(WEB_ISO_PUT_ERR_TIMEOUT,
                          web_iso_put_recv_all(&io1, dst, sizeof(dst), sizeof(src), 2000000ULL));
    TEST_ASSERT_EQUAL_UINT(1u, m1.script_idx);

    /* IO timeout after deadline has elapsed returns WEB_ISO_PUT_ERR_TIMEOUT immediately. */
    mock_put_io_t m2 = {
        .payload = src,
        .payload_len = sizeof(src),
        .script_ret = {WEB_ISO_PUT_IO_TIMEOUT, 6},
        .script_dt_us = {2000000ULL, 0ULL},
        .script_len = 2u,
        .now = 10ULL,
    };
    const web_iso_put_io_t io2 = make_io(&m2);
    TEST_ASSERT_EQUAL_INT(WEB_ISO_PUT_ERR_TIMEOUT,
                          web_iso_put_recv_all(&io2, dst, sizeof(dst), sizeof(src), 2000000ULL));
    TEST_ASSERT_EQUAL_UINT(1u, m2.script_idx);
}

void test_recv_all_io_errors_eof_and_overread(void) {
    uint8_t dst[8] = {0};
    mock_put_io_t m_eof = {
        .script_ret = {0},
        .script_len = 1u,
    };
    const web_iso_put_io_t io_eof = make_io(&m_eof);
    TEST_ASSERT_EQUAL_INT(WEB_ISO_PUT_ERR_IO,
                          web_iso_put_recv_all(&io_eof, dst, sizeof(dst), 4u, 1000000ULL));

    mock_put_io_t m_err = {
        .script_ret = {WEB_ISO_PUT_IO_ERROR},
        .script_len = 1u,
    };
    const web_iso_put_io_t io_err = make_io(&m_err);
    TEST_ASSERT_EQUAL_INT(WEB_ISO_PUT_ERR_IO,
                          web_iso_put_recv_all(&io_err, dst, sizeof(dst), 4u, 1000000ULL));

    mock_put_io_t m_over = {
        .script_ret = {5}, /* requested 4 bytes */
        .script_len = 1u,
    };
    const web_iso_put_io_t io_over = make_io(&m_over);
    TEST_ASSERT_EQUAL_INT(WEB_ISO_PUT_ERR_IO,
                          web_iso_put_recv_all(&io_over, dst, sizeof(dst), 4u, 1000000ULL));
}

static uint64_t backwards_now_cb(void *ctx) {
    uint64_t *seq = (uint64_t *)ctx;
    static size_t idx = 0u;
    const uint64_t val = seq[idx];
    idx = (idx + 1u) % 2u;
    return val;
}

static int dummy_recv_cb(void *ctx, uint8_t *buf, size_t max_len) {
    (void)ctx;
    (void)buf;
    (void)max_len;
    return WEB_ISO_PUT_IO_TIMEOUT;
}

void test_recv_all_backwards_clock_and_invalid_args(void) {
    uint8_t buf[8] = {0};
    mock_put_io_t m = {0};
    const web_iso_put_io_t valid_io = make_io(&m);

    TEST_ASSERT_EQUAL_INT(WEB_ISO_PUT_ERR_INVALID, web_iso_put_recv_all(NULL, buf, 8u, 4u, 1000ULL));
    TEST_ASSERT_EQUAL_INT(WEB_ISO_PUT_ERR_INVALID, web_iso_put_recv_all(&valid_io, NULL, 8u, 4u, 1000ULL));
    TEST_ASSERT_EQUAL_INT(WEB_ISO_PUT_ERR_INVALID, web_iso_put_recv_all(&valid_io, buf, 8u, 0u, 1000ULL));
    TEST_ASSERT_EQUAL_INT(WEB_ISO_PUT_ERR_INVALID, web_iso_put_recv_all(&valid_io, buf, 4u, 8u, 1000ULL));
    TEST_ASSERT_EQUAL_INT(WEB_ISO_PUT_ERR_INVALID, web_iso_put_recv_all(&valid_io, buf, 8u, 4u, 0ULL));

    web_iso_put_io_t missing_fn = valid_io;
    missing_fn.recv = NULL;
    TEST_ASSERT_EQUAL_INT(WEB_ISO_PUT_ERR_INVALID, web_iso_put_recv_all(&missing_fn, buf, 8u, 4u, 1000ULL));

    uint64_t clocks[2] = {1000ULL, 900ULL};
    const web_iso_put_io_t back_io = {
        .ctx = clocks,
        .now_us = backwards_now_cb,
        .set_timeout_us = NULL,
        .recv = dummy_recv_cb,
    };
    TEST_ASSERT_EQUAL_INT(WEB_ISO_PUT_ERR_TIMEOUT,
                          web_iso_put_recv_all(&back_io, buf, sizeof(buf), 4u, 1000000ULL));
}

void test_progress_5pct_milestones_and_suppression(void) {
    int bucket = -1;
    TEST_ASSERT_EQUAL_INT(-1, web_iso_progress_step_pct(0u, 1000u, &bucket));
    TEST_ASSERT_EQUAL_INT(-1, web_iso_progress_step_pct(49u, 1000u, &bucket));
    TEST_ASSERT_EQUAL_INT(5, web_iso_progress_step_pct(50u, 1000u, &bucket));
    TEST_ASSERT_EQUAL_INT(-1, web_iso_progress_step_pct(99u, 1000u, &bucket));
    TEST_ASSERT_EQUAL_INT(10, web_iso_progress_step_pct(100u, 1000u, &bucket));
    /* Repeated or regressed value emits -1. */
    TEST_ASSERT_EQUAL_INT(-1, web_iso_progress_step_pct(80u, 1000u, &bucket));
    TEST_ASSERT_EQUAL_INT(50, web_iso_progress_step_pct(520u, 1000u, &bucket));
    TEST_ASSERT_EQUAL_INT(100, web_iso_progress_step_pct(1000u, 1000u, &bucket));
    TEST_ASSERT_EQUAL_INT(-1, web_iso_progress_step_pct(1000u, 1000u, &bucket));
}

void test_progress_overflow_safe_large_sizes_and_invalid(void) {
    int bucket = -1;
    const uint64_t sixteen_gib = 16ULL * 1024ULL * 1024ULL * 1024ULL;
    TEST_ASSERT_EQUAL_INT(25, web_iso_progress_step_pct(sixteen_gib / 4ULL, sixteen_gib, &bucket));
    TEST_ASSERT_EQUAL_INT(100, web_iso_progress_step_pct(sixteen_gib, sixteen_gib, &bucket));

    int huge_bucket = -1;
    const uint64_t huge_total = UINT64_MAX - 99ULL;
    TEST_ASSERT_EQUAL_INT(50, web_iso_progress_step_pct(huge_total / 2ULL, huge_total, &huge_bucket));
    TEST_ASSERT_EQUAL_INT(100, web_iso_progress_step_pct(huge_total, huge_total, &huge_bucket));

    TEST_ASSERT_EQUAL_INT(-1, web_iso_progress_step_pct(10u, 0u, &bucket));
    TEST_ASSERT_EQUAL_INT(-1, web_iso_progress_step_pct(10u, 100u, NULL));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_recv_all_single_chunk_exact);
    RUN_TEST(test_recv_all_partial_reads_and_min_slice_clamp);
    RUN_TEST(test_recv_all_timeout_before_and_after_recv);
    RUN_TEST(test_recv_all_io_errors_eof_and_overread);
    RUN_TEST(test_recv_all_backwards_clock_and_invalid_args);
    RUN_TEST(test_progress_5pct_milestones_and_suppression);
    RUN_TEST(test_progress_overflow_safe_large_sizes_and_invalid);
    return UNITY_END();
}
