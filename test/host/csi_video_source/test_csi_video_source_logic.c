#include "unity.h"
#include "csi_video_source_logic.h"

void setUp(void) {}
void tearDown(void) {}

// --- 1. JPEG frame validation (SOI / EOI / Frame Hub max_frame_size) ---

static void test_jpeg_check_frame_empty_and_null(void)
{
    const uint8_t minimal[4] = {0xFF, 0xD8, 0xFF, 0xD9};
    TEST_ASSERT_EQUAL_INT(CSI_JPEG_FRAME_ERR_EMPTY,
                          csi_jpeg_check_frame(NULL, 4, 1024));
    TEST_ASSERT_EQUAL_INT(CSI_JPEG_FRAME_ERR_EMPTY,
                          csi_jpeg_check_frame(minimal, 0, 1024));
    TEST_ASSERT_EQUAL_INT(CSI_JPEG_FRAME_ERR_EMPTY,
                          csi_jpeg_check_frame(minimal, 3, 1024));
}

static void test_jpeg_check_frame_markers(void)
{
    const uint8_t minimal_ok[4] = {0xFF, 0xD8, 0xFF, 0xD9};
    TEST_ASSERT_EQUAL_INT(CSI_JPEG_FRAME_OK,
                          csi_jpeg_check_frame(minimal_ok, 4, 4));

    const uint8_t bad_soi0[6] = {0xFE, 0xD8, 0x00, 0x01, 0xFF, 0xD9};
    const uint8_t bad_soi1[6] = {0xFF, 0xD9, 0x00, 0x01, 0xFF, 0xD9};
    const uint8_t bad_eoi0[6] = {0xFF, 0xD8, 0x00, 0x01, 0xFE, 0xD9};
    const uint8_t bad_eoi1[6] = {0xFF, 0xD8, 0x00, 0x01, 0xFF, 0xD8};

    TEST_ASSERT_EQUAL_INT(CSI_JPEG_FRAME_ERR_MARKERS,
                          csi_jpeg_check_frame(bad_soi0, sizeof(bad_soi0), 1024));
    TEST_ASSERT_EQUAL_INT(CSI_JPEG_FRAME_ERR_MARKERS,
                          csi_jpeg_check_frame(bad_soi1, sizeof(bad_soi1), 1024));
    TEST_ASSERT_EQUAL_INT(CSI_JPEG_FRAME_ERR_MARKERS,
                          csi_jpeg_check_frame(bad_eoi0, sizeof(bad_eoi0), 1024));
    TEST_ASSERT_EQUAL_INT(CSI_JPEG_FRAME_ERR_MARKERS,
                          csi_jpeg_check_frame(bad_eoi1, sizeof(bad_eoi1), 1024));
}

static void test_jpeg_check_frame_oversize_boundary(void)
{
    uint8_t frame[8] = {0xFF, 0xD8, 0x11, 0x22, 0x33, 0x44, 0xFF, 0xD9};

    // Exact boundary len == max_frame_bytes must pass.
    TEST_ASSERT_EQUAL_INT(CSI_JPEG_FRAME_OK,
                          csi_jpeg_check_frame(frame, 8, 8));
    // One byte over max_frame_bytes must be classified as OVERSIZE (INTERFACES.md §1.2(b)).
    TEST_ASSERT_EQUAL_INT(CSI_JPEG_FRAME_ERR_OVERSIZE,
                          csi_jpeg_check_frame(frame, 8, 7));
    TEST_ASSERT_EQUAL_INT(CSI_JPEG_FRAME_ERR_OVERSIZE,
                          csi_jpeg_check_frame(frame, 8, 0));
}

// --- 2. Reusable JPEG encoder buffer sizing ---

static void test_jpeg_calc_output_buf_size(void)
{
    TEST_ASSERT_EQUAL_UINT(0u, csi_jpeg_calc_output_buf_size(0u));
    TEST_ASSERT_EQUAL_UINT(0u, csi_jpeg_calc_output_buf_size(CSI_JPEG_MAX_OUT_BYTES + 1u));

    // Small hub caps raise to 512 KiB minimum recommended buffer.
    TEST_ASSERT_EQUAL_UINT(CSI_JPEG_MIN_RECOMMENDED_OUT,
                           csi_jpeg_calc_output_buf_size(64u * 1024u));
    // Default 256 KiB Frame Hub -> 512 KiB encoder buffer.
    TEST_ASSERT_EQUAL_UINT(512u * 1024u,
                           csi_jpeg_calc_output_buf_size(256u * 1024u));
    // 300 KiB Frame Hub -> 600 KiB encoder buffer (2x headroom).
    TEST_ASSERT_EQUAL_UINT(600u * 1024u,
                           csi_jpeg_calc_output_buf_size(300u * 1024u));
    // Exact half-cap and above clamp to CSI_JPEG_MAX_OUT_BYTES (4 MiB).
    TEST_ASSERT_EQUAL_UINT(CSI_JPEG_MAX_OUT_BYTES,
                           csi_jpeg_calc_output_buf_size(CSI_JPEG_MAX_OUT_BYTES / 2u));
    TEST_ASSERT_EQUAL_UINT(CSI_JPEG_MAX_OUT_BYTES,
                           csi_jpeg_calc_output_buf_size(CSI_JPEG_MAX_OUT_BYTES / 2u + 1u));
    TEST_ASSERT_EQUAL_UINT(CSI_JPEG_MAX_OUT_BYTES,
                           csi_jpeg_calc_output_buf_size(CSI_JPEG_MAX_OUT_BYTES));
}

// --- 3. Lock-free seqlock status store (INTERFACES.md §1.3) ---

static void test_seqlock_init_and_state_normalization(void)
{
    csi_video_seq_t seq;
    csi_video_status_t out = {.state = KVM_VIDEO_SYNC_LOCKED, .width = 999, .height = 999};

    csi_video_seq_init(&seq);
    TEST_ASSERT_TRUE(csi_video_seq_read(&seq, CSI_VIDEO_SEQ_DEFAULT_RETRIES, &out));
    TEST_ASSERT_EQUAL_INT(KVM_VIDEO_SYNC_UNKNOWN, out.state);
    TEST_ASSERT_EQUAL_UINT32(0u, out.width);
    TEST_ASSERT_EQUAL_UINT32(0u, out.height);

    // LOCKED preserves width and height.
    csi_video_seq_write(&seq, KVM_VIDEO_SYNC_LOCKED, 1280u, 720u);
    TEST_ASSERT_TRUE(csi_video_seq_read(&seq, CSI_VIDEO_SEQ_DEFAULT_RETRIES, &out));
    TEST_ASSERT_EQUAL_INT(KVM_VIDEO_SYNC_LOCKED, out.state);
    TEST_ASSERT_EQUAL_UINT32(1280u, out.width);
    TEST_ASSERT_EQUAL_UINT32(720u, out.height);

    // NO_SIGNAL forces width and height to 0 (INTERFACES.md §1.3).
    csi_video_seq_write(&seq, KVM_VIDEO_SYNC_NO_SIGNAL, 1280u, 720u);
    TEST_ASSERT_TRUE(csi_video_seq_read(&seq, CSI_VIDEO_SEQ_DEFAULT_RETRIES, &out));
    TEST_ASSERT_EQUAL_INT(KVM_VIDEO_SYNC_NO_SIGNAL, out.state);
    TEST_ASSERT_EQUAL_UINT32(0u, out.width);
    TEST_ASSERT_EQUAL_UINT32(0u, out.height);

    // LOCKED with 0 width or 0 height is invalid -> normalized to UNKNOWN (0, 0).
    csi_video_seq_write(&seq, KVM_VIDEO_SYNC_LOCKED, 0u, 720u);
    TEST_ASSERT_TRUE(csi_video_seq_read(&seq, CSI_VIDEO_SEQ_DEFAULT_RETRIES, &out));
    TEST_ASSERT_EQUAL_INT(KVM_VIDEO_SYNC_UNKNOWN, out.state);
    TEST_ASSERT_EQUAL_UINT32(0u, out.width);
    TEST_ASSERT_EQUAL_UINT32(0u, out.height);

    csi_video_seq_write(&seq, KVM_VIDEO_SYNC_LOCKED, 1280u, 0u);
    TEST_ASSERT_TRUE(csi_video_seq_read(&seq, CSI_VIDEO_SEQ_DEFAULT_RETRIES, &out));
    TEST_ASSERT_EQUAL_INT(KVM_VIDEO_SYNC_UNKNOWN, out.state);
    TEST_ASSERT_EQUAL_UINT32(0u, out.width);
    TEST_ASSERT_EQUAL_UINT32(0u, out.height);

    // Out-of-range enum value -> UNKNOWN (0, 0).
    csi_video_seq_write(&seq, (kvm_video_sync_state_t)77, 640u, 480u);
    TEST_ASSERT_TRUE(csi_video_seq_read(&seq, CSI_VIDEO_SEQ_DEFAULT_RETRIES, &out));
    TEST_ASSERT_EQUAL_INT(KVM_VIDEO_SYNC_UNKNOWN, out.state);
    TEST_ASSERT_EQUAL_UINT32(0u, out.width);
    TEST_ASSERT_EQUAL_UINT32(0u, out.height);
}

static void test_seqlock_writer_in_progress_exhausts_retries_to_unknown(void)
{
    csi_video_seq_t seq;
    csi_video_seq_init(&seq);
    csi_video_seq_write(&seq, KVM_VIDEO_SYNC_LOCKED, 1280u, 720u);

    // Writer begins an update (seq becomes odd) and has not finished yet.
    csi_video_seq_begin_write(&seq);
    TEST_ASSERT_EQUAL_UINT32(1u, seq.seq & 1u);

    csi_video_status_t out = {.state = KVM_VIDEO_SYNC_LOCKED, .width = 1280u, .height = 720u};
    TEST_ASSERT_FALSE(csi_video_seq_read(&seq, 4u, &out));
    TEST_ASSERT_EQUAL_INT(KVM_VIDEO_SYNC_UNKNOWN, out.state);
    TEST_ASSERT_EQUAL_UINT32(0u, out.width);
    TEST_ASSERT_EQUAL_UINT32(0u, out.height);

    // Writer completes update (seq becomes even again) -> reader succeeds immediately.
    csi_video_seq_end_write(&seq, KVM_VIDEO_SYNC_LOCKED, 1920u, 1080u);
    TEST_ASSERT_EQUAL_UINT32(0u, seq.seq & 1u);
    TEST_ASSERT_TRUE(csi_video_seq_read(&seq, 1u, &out));
    TEST_ASSERT_EQUAL_INT(KVM_VIDEO_SYNC_LOCKED, out.state);
    TEST_ASSERT_EQUAL_UINT32(1920u, out.width);
    TEST_ASSERT_EQUAL_UINT32(1080u, out.height);
}

typedef struct {
    csi_video_seq_t *seq;
    uint32_t max_interferences;
    uint32_t calls;
} mid_read_interferer_t;

static void interfere_with_complete_write(void *ctx)
{
    mid_read_interferer_t *m = (mid_read_interferer_t *)ctx;
    m->calls++;
    if (m->calls <= m->max_interferences) {
        // Complete an entire write cycle so seq jumps from even 2k to even 2k+2!
        csi_video_seq_write(m->seq, KVM_VIDEO_SYNC_LOCKED, 1920u, 1080u);
    }
}

static void test_seqlock_detects_even_to_even_generation_jump_and_retries(void)
{
    csi_video_seq_t seq;
    csi_video_seq_init(&seq);
    csi_video_seq_write(&seq, KVM_VIDEO_SYNC_LOCKED, 1280u, 720u);

    // 1 interference with max_retries == 1 must fail (proves s1 == s2 check, not merely even check!).
    mid_read_interferer_t m1 = {.seq = &seq, .max_interferences = 1u, .calls = 0u};
    csi_video_status_t out = {.state = KVM_VIDEO_SYNC_LOCKED, .width = 1280u, .height = 720u};
    TEST_ASSERT_FALSE(csi_video_seq_read_ex(&seq, 1u, interfere_with_complete_write, &m1, &out));
    TEST_ASSERT_EQUAL_UINT32(1u, m1.calls);
    TEST_ASSERT_EQUAL_INT(KVM_VIDEO_SYNC_UNKNOWN, out.state);
    TEST_ASSERT_EQUAL_UINT32(0u, out.width);
    TEST_ASSERT_EQUAL_UINT32(0u, out.height);

    // Reset to 1280x720; 1 interference with max_retries == 2 must retry and return 1920x1080!
    csi_video_seq_write(&seq, KVM_VIDEO_SYNC_LOCKED, 1280u, 720u);
    mid_read_interferer_t m2 = {.seq = &seq, .max_interferences = 1u, .calls = 0u};
    TEST_ASSERT_TRUE(csi_video_seq_read_ex(&seq, 2u, interfere_with_complete_write, &m2, &out));
    TEST_ASSERT_EQUAL_UINT32(2u, m2.calls);
    TEST_ASSERT_EQUAL_INT(KVM_VIDEO_SYNC_LOCKED, out.state);
    TEST_ASSERT_EQUAL_UINT32(1920u, out.width);
    TEST_ASSERT_EQUAL_UINT32(1080u, out.height);
}

static void test_seqlock_null_and_zero_retries(void)
{
    csi_video_seq_t seq;
    csi_video_seq_init(&seq);
    csi_video_seq_write(&seq, KVM_VIDEO_SYNC_LOCKED, 1280u, 720u);

    TEST_ASSERT_FALSE(csi_video_seq_read(&seq, 4u, NULL));

    csi_video_status_t out = {.state = KVM_VIDEO_SYNC_LOCKED, .width = 1280u, .height = 720u};
    TEST_ASSERT_FALSE(csi_video_seq_read(NULL, 4u, &out));
    TEST_ASSERT_EQUAL_INT(KVM_VIDEO_SYNC_UNKNOWN, out.state);
    TEST_ASSERT_EQUAL_UINT32(0u, out.width);
    TEST_ASSERT_EQUAL_UINT32(0u, out.height);

    out.state = KVM_VIDEO_SYNC_LOCKED;
    out.width = 1280u;
    out.height = 720u;
    TEST_ASSERT_FALSE(csi_video_seq_read(&seq, 0u, &out));
    TEST_ASSERT_EQUAL_INT(KVM_VIDEO_SYNC_UNKNOWN, out.state);
    TEST_ASSERT_EQUAL_UINT32(0u, out.width);
    TEST_ASSERT_EQUAL_UINT32(0u, out.height);
}

// --- 4. Publish / drop / latency statistics ---

static void test_pub_stats_accounting_and_window_reset(void)
{
    csi_video_pub_stats_t st;
    csi_video_pub_stats_init(&st);

    csi_video_pub_stats_on_published(&st, 50000u, 9000u);
    csi_video_pub_stats_on_pool_busy(&st, 42000u, 8500u);
    csi_video_pub_stats_on_oversize(&st, 280000u, 11200u);
    csi_video_pub_stats_on_error(&st);

    TEST_ASSERT_EQUAL_UINT64(1u, st.published);
    TEST_ASSERT_EQUAL_UINT64(1u, st.pool_busy_drops);
    TEST_ASSERT_EQUAL_UINT64(1u, st.oversize_drops);
    TEST_ASSERT_EQUAL_UINT64(1u, st.encode_errors);

    TEST_ASSERT_EQUAL_UINT32(3u, st.win_frames);
    TEST_ASSERT_EQUAL_UINT64(9000u + 8500u + 11200u, st.win_encode_us);
    TEST_ASSERT_EQUAL_UINT32(11200u, st.win_max_encode_us);
    TEST_ASSERT_EQUAL_UINT64(50000u + 42000u + 280000u, st.win_jpeg_bytes);
    TEST_ASSERT_EQUAL_UINT(42000u, st.win_min_jpeg_bytes);
    TEST_ASSERT_EQUAL_UINT(280000u, st.win_max_jpeg_bytes);
    TEST_ASSERT_EQUAL_UINT(280000u, st.peak_jpeg_bytes);

    // Window reset clears window counters while preserving cumulative totals and all-time peak.
    csi_video_pub_stats_reset_window(&st);
    TEST_ASSERT_EQUAL_UINT32(0u, st.win_frames);
    TEST_ASSERT_EQUAL_UINT64(0u, st.win_encode_us);
    TEST_ASSERT_EQUAL_UINT32(0u, st.win_max_encode_us);
    TEST_ASSERT_EQUAL_UINT64(0u, st.win_jpeg_bytes);
    TEST_ASSERT_EQUAL_UINT(0u, st.win_min_jpeg_bytes);
    TEST_ASSERT_EQUAL_UINT(0u, st.win_max_jpeg_bytes);
    TEST_ASSERT_EQUAL_UINT(280000u, st.peak_jpeg_bytes);
    TEST_ASSERT_EQUAL_UINT64(1u, st.published);
    TEST_ASSERT_EQUAL_UINT64(1u, st.pool_busy_drops);
    TEST_ASSERT_EQUAL_UINT64(1u, st.oversize_drops);
    TEST_ASSERT_EQUAL_UINT64(1u, st.encode_errors);

    // New frame in next window sets new window min/max without losing higher peak_jpeg_bytes.
    csi_video_pub_stats_on_published(&st, 60000u, 7800u);
    TEST_ASSERT_EQUAL_UINT(60000u, st.win_min_jpeg_bytes);
    TEST_ASSERT_EQUAL_UINT(60000u, st.win_max_jpeg_bytes);
    TEST_ASSERT_EQUAL_UINT(280000u, st.peak_jpeg_bytes);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_jpeg_check_frame_empty_and_null);
    RUN_TEST(test_jpeg_check_frame_markers);
    RUN_TEST(test_jpeg_check_frame_oversize_boundary);
    RUN_TEST(test_jpeg_calc_output_buf_size);
    RUN_TEST(test_seqlock_init_and_state_normalization);
    RUN_TEST(test_seqlock_writer_in_progress_exhausts_retries_to_unknown);
    RUN_TEST(test_seqlock_detects_even_to_even_generation_jump_and_retries);
    RUN_TEST(test_seqlock_null_and_zero_retries);
    RUN_TEST(test_pub_stats_accounting_and_window_reset);
    return UNITY_END();
}
