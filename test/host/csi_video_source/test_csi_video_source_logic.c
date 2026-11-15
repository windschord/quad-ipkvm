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

// --- 5. P2b-7 Mode stability, resolution switch, HPD nudge & stall recovery ---

static csi_mode_sample_t make_sample(uint64_t now_ms, bool ddc5v, bool sync_locked,
                                     bool geom_ok, uint32_t w, uint32_t h)
{
    csi_mode_sample_t s = {
        .now_ms = now_ms,
        .ddc5v = ddc5v,
        .sync_locked = sync_locked,
        .geom_ok = geom_ok,
        .width = w,
        .height = h,
    };
    return s;
}

static void test_mode_tracker_two_sample_stability_and_jitter(void)
{
    csi_mode_tracker_t m;
    csi_mode_tracker_init(&m);

    TEST_ASSERT_EQUAL_INT(CSI_MODE_ACT_WAIT, csi_mode_tracker_step(NULL, NULL));
    csi_mode_sample_t s0 = make_sample(100u, true, true, true, 1280u, 720u);
    TEST_ASSERT_EQUAL_INT(CSI_MODE_ACT_WAIT, csi_mode_tracker_step(&m, NULL));

    // Sample #1 of 1280x720 -> WAIT (must NOT start on single transient read)
    TEST_ASSERT_EQUAL_INT(CSI_MODE_ACT_WAIT, csi_mode_tracker_step(&m, &s0));
    TEST_ASSERT_FALSE(m.streaming);

    // Transient jitter to 1920x1080 resets candidate count to 1 -> WAIT
    csi_mode_sample_t s1 = make_sample(300u, true, true, true, 1920u, 1080u);
    TEST_ASSERT_EQUAL_INT(CSI_MODE_ACT_WAIT, csi_mode_tracker_step(&m, &s1));
    TEST_ASSERT_FALSE(m.streaming);
    TEST_ASSERT_EQUAL_UINT32(1u, m.cand_hits);

    // Second consecutive 1920x1080 -> START_MODE!
    csi_mode_sample_t s2 = make_sample(500u, true, true, true, 1920u, 1080u);
    TEST_ASSERT_EQUAL_INT(CSI_MODE_ACT_START_MODE, csi_mode_tracker_step(&m, &s2));
    TEST_ASSERT_TRUE(m.streaming);
    TEST_ASSERT_EQUAL_UINT32(1920u, m.active_w);
    TEST_ASSERT_EQUAL_UINT32(1080u, m.active_h);
    TEST_ASSERT_EQUAL_UINT32(1u, m.mode_switches);
    TEST_ASSERT_EQUAL_UINT64(500u, m.last_frame_ms);

    // Steady-state polling at same resolution -> KEEP_STREAMING
    csi_mode_sample_t s3 = make_sample(700u, true, true, true, 1920u, 1080u);
    TEST_ASSERT_EQUAL_INT(CSI_MODE_ACT_KEEP_STREAMING, csi_mode_tracker_step(&m, &s3));
    TEST_ASSERT_EQUAL_UINT32(1u, m.mode_switches);
}

static void test_mode_tracker_live_resolution_switch_and_signal_loss(void)
{
    csi_mode_tracker_t m;
    csi_mode_tracker_init(&m);

    // Lock onto 1920x1080
    csi_mode_sample_t a1 = make_sample(0u, true, true, true, 1920u, 1080u);
    csi_mode_sample_t a2 = make_sample(200u, true, true, true, 1920u, 1080u);
    TEST_ASSERT_EQUAL_INT(CSI_MODE_ACT_WAIT, csi_mode_tracker_step(&m, &a1));
    TEST_ASSERT_EQUAL_INT(CSI_MODE_ACT_START_MODE, csi_mode_tracker_step(&m, &a2));

    // Switch to 1280x720: first sample stops CSI immediately and records hit #1
    csi_mode_sample_t b1 = make_sample(400u, true, true, true, 1280u, 720u);
    TEST_ASSERT_EQUAL_INT(CSI_MODE_ACT_STOP_MODE_CHANGED, csi_mode_tracker_step(&m, &b1));
    TEST_ASSERT_FALSE(m.streaming);
    TEST_ASSERT_EQUAL_UINT32(0u, m.active_w);
    TEST_ASSERT_EQUAL_UINT32(1280u, m.cand_w);
    TEST_ASSERT_EQUAL_UINT32(720u, m.cand_h);
    TEST_ASSERT_EQUAL_UINT32(1u, m.cand_hits);

    // Second consecutive 1280x720 sample starts CSI at 1280x720
    csi_mode_sample_t b2 = make_sample(600u, true, true, true, 1280u, 720u);
    TEST_ASSERT_EQUAL_INT(CSI_MODE_ACT_START_MODE, csi_mode_tracker_step(&m, &b2));
    TEST_ASSERT_TRUE(m.streaming);
    TEST_ASSERT_EQUAL_UINT32(1280u, m.active_w);
    TEST_ASSERT_EQUAL_UINT32(720u, m.active_h);
    TEST_ASSERT_EQUAL_UINT32(2u, m.mode_switches);

    // Switch to 640x480 and verify height-only change (e.g. 640x480 -> 640x400) is also caught
    csi_mode_sample_t c1 = make_sample(800u, true, true, true, 640u, 480u);
    csi_mode_sample_t c2 = make_sample(1000u, true, true, true, 640u, 480u);
    TEST_ASSERT_EQUAL_INT(CSI_MODE_ACT_STOP_MODE_CHANGED, csi_mode_tracker_step(&m, &c1));
    TEST_ASSERT_EQUAL_INT(CSI_MODE_ACT_START_MODE, csi_mode_tracker_step(&m, &c2));
    csi_mode_sample_t c3 = make_sample(1200u, true, true, true, 640u, 400u);
    TEST_ASSERT_EQUAL_INT(CSI_MODE_ACT_STOP_MODE_CHANGED, csi_mode_tracker_step(&m, &c3));
    csi_mode_sample_t c4 = make_sample(1400u, true, true, true, 640u, 400u);
    TEST_ASSERT_EQUAL_INT(CSI_MODE_ACT_START_MODE, csi_mode_tracker_step(&m, &c4));
    TEST_ASSERT_EQUAL_UINT32(4u, m.mode_switches);

    // Signal loss while streaming -> STOP_NO_SIGNAL
    csi_mode_sample_t lost = make_sample(1600u, true, false, false, 0u, 0u);
    TEST_ASSERT_EQUAL_INT(CSI_MODE_ACT_STOP_NO_SIGNAL, csi_mode_tracker_step(&m, &lost));
    TEST_ASSERT_FALSE(m.streaming);
    TEST_ASSERT_EQUAL_UINT32(0u, m.active_w);
    TEST_ASSERT_EQUAL_UINT32(0u, m.active_h);
}

static void test_mode_tracker_out_of_range_and_abort_start(void)
{
    csi_mode_tracker_t m;
    csi_mode_tracker_init(&m);

    // Out-of-range mode while unlocked -> REJECT_OUT_OF_RANGE, never starts
    csi_mode_sample_t bad = make_sample(100u, true, true, false, 3840u, 2160u);
    TEST_ASSERT_EQUAL_INT(CSI_MODE_ACT_REJECT_OUT_OF_RANGE, csi_mode_tracker_step(&m, &bad));
    TEST_ASSERT_EQUAL_INT(CSI_MODE_ACT_REJECT_OUT_OF_RANGE, csi_mode_tracker_step(&m, &bad));
    TEST_ASSERT_FALSE(m.streaming);

    // Zero width/height even if geom_ok were true -> REJECT_OUT_OF_RANGE
    csi_mode_sample_t zero_w = make_sample(200u, true, true, true, 0u, 720u);
    csi_mode_sample_t zero_h = make_sample(300u, true, true, true, 1280u, 0u);
    TEST_ASSERT_EQUAL_INT(CSI_MODE_ACT_REJECT_OUT_OF_RANGE, csi_mode_tracker_step(&m, &zero_w));
    TEST_ASSERT_EQUAL_INT(CSI_MODE_ACT_REJECT_OUT_OF_RANGE, csi_mode_tracker_step(&m, &zero_h));

    // Lock valid mode, then switch to out-of-range -> REJECT_OUT_OF_RANGE & stops streaming
    csi_mode_sample_t ok1 = make_sample(400u, true, true, true, 1280u, 720u);
    csi_mode_sample_t ok2 = make_sample(600u, true, true, true, 1280u, 720u);
    TEST_ASSERT_EQUAL_INT(CSI_MODE_ACT_WAIT, csi_mode_tracker_step(&m, &ok1));
    TEST_ASSERT_EQUAL_INT(CSI_MODE_ACT_START_MODE, csi_mode_tracker_step(&m, &ok2));
    TEST_ASSERT_TRUE(m.streaming);

    csi_mode_sample_t bad2 = make_sample(800u, true, true, false, 2560u, 1440u);
    TEST_ASSERT_EQUAL_INT(CSI_MODE_ACT_REJECT_OUT_OF_RANGE, csi_mode_tracker_step(&m, &bad2));
    TEST_ASSERT_FALSE(m.streaming);

    // Start again, then simulate driver failure via csi_mode_tracker_abort_start
    TEST_ASSERT_EQUAL_INT(CSI_MODE_ACT_WAIT, csi_mode_tracker_step(&m, &ok1));
    TEST_ASSERT_EQUAL_INT(CSI_MODE_ACT_START_MODE, csi_mode_tracker_step(&m, &ok2));
    csi_mode_tracker_abort_start(&m);
    TEST_ASSERT_FALSE(m.streaming);
    TEST_ASSERT_EQUAL_UINT32(0u, m.active_w);
    TEST_ASSERT_EQUAL_UINT32(0u, m.cand_hits);
}

static void test_mode_tracker_hpd_nudge_10_20_40_backoff(void)
{
    csi_mode_tracker_t m;
    csi_mode_tracker_init(&m);

    // Without DDC5V (cable unplugged), HPD must NEVER be nudged even after 60s
    csi_mode_sample_t unplug0 = make_sample(0u, false, false, false, 0u, 0u);
    csi_mode_sample_t unplug60 = make_sample(60000u, false, false, false, 0u, 0u);
    TEST_ASSERT_EQUAL_INT(CSI_MODE_ACT_WAIT, csi_mode_tracker_step(&m, &unplug0));
    TEST_ASSERT_EQUAL_INT(CSI_MODE_ACT_WAIT, csi_mode_tracker_step(&m, &unplug60));
    TEST_ASSERT_EQUAL_UINT32(0u, m.hpd_nudges);

    // DDC5V appears at t = 1000 ms without sync: timer arms
    csi_mode_sample_t d0 = make_sample(1000u, true, false, false, 0u, 0u);
    TEST_ASSERT_EQUAL_INT(CSI_MODE_ACT_WAIT, csi_mode_tracker_step(&m, &d0));

    // At +9999 ms (t = 10999): still WAIT; at +10000 ms (t = 11000): NUDGE #1
    csi_mode_sample_t d1_pre = make_sample(10999u, true, false, false, 0u, 0u);
    csi_mode_sample_t d1_hit = make_sample(11000u, true, false, false, 0u, 0u);
    TEST_ASSERT_EQUAL_INT(CSI_MODE_ACT_WAIT, csi_mode_tracker_step(&m, &d1_pre));
    TEST_ASSERT_EQUAL_INT(CSI_MODE_ACT_NUDGE_HPD, csi_mode_tracker_step(&m, &d1_hit));
    TEST_ASSERT_EQUAL_UINT32(1u, m.hpd_nudges);

    // Stage 2 backoff = 20000 ms (from 11000 -> 31000)
    csi_mode_sample_t d2_pre = make_sample(30999u, true, false, false, 0u, 0u);
    csi_mode_sample_t d2_hit = make_sample(31000u, true, false, false, 0u, 0u);
    TEST_ASSERT_EQUAL_INT(CSI_MODE_ACT_WAIT, csi_mode_tracker_step(&m, &d2_pre));
    TEST_ASSERT_EQUAL_INT(CSI_MODE_ACT_NUDGE_HPD, csi_mode_tracker_step(&m, &d2_hit));
    TEST_ASSERT_EQUAL_UINT32(2u, m.hpd_nudges);

    // Stage 3 backoff = 40000 ms (from 31000 -> 71000)
    csi_mode_sample_t d3_pre = make_sample(70999u, true, false, false, 0u, 0u);
    csi_mode_sample_t d3_hit = make_sample(71000u, true, false, false, 0u, 0u);
    TEST_ASSERT_EQUAL_INT(CSI_MODE_ACT_WAIT, csi_mode_tracker_step(&m, &d3_pre));
    TEST_ASSERT_EQUAL_INT(CSI_MODE_ACT_NUDGE_HPD, csi_mode_tracker_step(&m, &d3_hit));
    TEST_ASSERT_EQUAL_UINT32(3u, m.hpd_nudges);

    // After 3 nudges, stop nudging indefinitely while cable remains idle
    csi_mode_sample_t d4 = make_sample(200000u, true, false, false, 0u, 0u);
    TEST_ASSERT_EQUAL_INT(CSI_MODE_ACT_WAIT, csi_mode_tracker_step(&m, &d4));
    TEST_ASSERT_EQUAL_UINT32(3u, m.hpd_nudges);

    // Cable unplug resets backoff so re-plugging gets fresh 10s nudge
    csi_mode_sample_t replug_off = make_sample(201000u, false, false, false, 0u, 0u);
    csi_mode_sample_t replug_on  = make_sample(202000u, true, false, false, 0u, 0u);
    csi_mode_sample_t replug_10s = make_sample(212000u, true, false, false, 0u, 0u);
    TEST_ASSERT_EQUAL_INT(CSI_MODE_ACT_WAIT, csi_mode_tracker_step(&m, &replug_off));
    TEST_ASSERT_EQUAL_INT(CSI_MODE_ACT_WAIT, csi_mode_tracker_step(&m, &replug_on));
    TEST_ASSERT_EQUAL_INT(CSI_MODE_ACT_NUDGE_HPD, csi_mode_tracker_step(&m, &replug_10s));
    TEST_ASSERT_EQUAL_UINT32(4u, m.hpd_nudges);
}

static void test_mode_tracker_stall_recovery_and_cooldown(void)
{
    csi_mode_tracker_t m;
    csi_mode_tracker_init(&m);

    // Not streaming -> WAIT
    TEST_ASSERT_EQUAL_INT(CSI_MODE_ACT_WAIT, csi_mode_tracker_check_stall(&m, 5000u));

    csi_mode_sample_t s1 = make_sample(1000u, true, true, true, 1280u, 720u);
    csi_mode_sample_t s2 = make_sample(1200u, true, true, true, 1280u, 720u);
    (void)csi_mode_tracker_step(&m, &s1);
    TEST_ASSERT_EQUAL_INT(CSI_MODE_ACT_START_MODE, csi_mode_tracker_step(&m, &s2));

    // Frame arrives at 2500 ms -> stall deadline moves to 4500 ms
    csi_mode_tracker_on_frame(&m, 2500u);
    TEST_ASSERT_EQUAL_INT(CSI_MODE_ACT_KEEP_STREAMING, csi_mode_tracker_check_stall(&m, 4499u));
    TEST_ASSERT_EQUAL_INT(CSI_MODE_ACT_RECOVER_STALL, csi_mode_tracker_check_stall(&m, 4500u));
    TEST_ASSERT_FALSE(m.streaming);
    TEST_ASSERT_EQUAL_UINT32(1u, m.stall_recoveries);

    // Re-lock at 5000 ms; stall at 7000 ms is within 8000 ms cooldown (4500 + 8000 = 12500),
    // so recovery must NOT fire until 12500 ms!
    csi_mode_sample_t r1 = make_sample(4800u, true, true, true, 1280u, 720u);
    csi_mode_sample_t r2 = make_sample(5000u, true, true, true, 1280u, 720u);
    (void)csi_mode_tracker_step(&m, &r1);
    TEST_ASSERT_EQUAL_INT(CSI_MODE_ACT_START_MODE, csi_mode_tracker_step(&m, &r2));

    TEST_ASSERT_EQUAL_INT(CSI_MODE_ACT_KEEP_STREAMING, csi_mode_tracker_check_stall(&m, 7000u));
    TEST_ASSERT_EQUAL_INT(CSI_MODE_ACT_KEEP_STREAMING, csi_mode_tracker_check_stall(&m, 12499u));
    TEST_ASSERT_EQUAL_INT(CSI_MODE_ACT_RECOVER_STALL, csi_mode_tracker_check_stall(&m, 12500u));
    TEST_ASSERT_EQUAL_UINT32(2u, m.stall_recoveries);

    // Not streaming: csi_mode_tracker_on_frame MUST NOT update last_frame_ms (#82 item 11).
    TEST_ASSERT_FALSE(m.streaming);
    uint64_t prev_last_frame = m.last_frame_ms;
    csi_mode_tracker_on_frame(&m, 999999u);
    TEST_ASSERT_EQUAL_UINT64(prev_last_frame, m.last_frame_ms);
    csi_mode_tracker_on_frame(NULL, 999999u);
}

static void test_mode_tracker_poll_skips_step_and_stall_on_i2c_failure(void)
{
    csi_mode_tracker_t m;
    csi_mode_tracker_init(&m);

    TEST_ASSERT_EQUAL_INT(CSI_MODE_ACT_WAIT, csi_mode_tracker_poll(NULL, true, NULL));

    // 1. Unlocked with 1 candidate hit: transient I2C read failure (sample_ok = false)
    //    must return WAIT without resetting cand_hits, so the next valid sample starts CSI!
    csi_mode_sample_t s1 = make_sample(1000u, true, true, true, 1280u, 720u);
    TEST_ASSERT_EQUAL_INT(CSI_MODE_ACT_WAIT, csi_mode_tracker_poll(&m, true, &s1));
    TEST_ASSERT_EQUAL_UINT32(1u, m.cand_hits);

    csi_mode_sample_t zero_sample = make_sample(1200u, false, false, false, 0u, 0u);
    TEST_ASSERT_EQUAL_INT(CSI_MODE_ACT_WAIT, csi_mode_tracker_poll(&m, false, &zero_sample));
    TEST_ASSERT_EQUAL_UINT32(1u, m.sample_errors);
    TEST_ASSERT_EQUAL_UINT32(1u, m.cand_hits);
    TEST_ASSERT_FALSE(m.streaming);

    csi_mode_sample_t s2 = make_sample(1400u, true, true, true, 1280u, 720u);
    TEST_ASSERT_EQUAL_INT(CSI_MODE_ACT_START_MODE, csi_mode_tracker_poll(&m, true, &s2));
    TEST_ASSERT_TRUE(m.streaming);
    TEST_ASSERT_EQUAL_UINT32(1280u, m.active_w);
    TEST_ASSERT_EQUAL_UINT32(720u, m.active_h);

    // 2. Streaming: even if zero_sample looks like NO_SIGNAL AND now_ms (5000) exceeds the
    //    2000 ms stall threshold (1400 + 2000 = 3400), sample_ok == false MUST NOT call
    //    csi_mode_tracker_step() or csi_mode_tracker_check_stall() — stream stays alive!
    csi_mode_sample_t failed_poll = make_sample(5000u, false, false, false, 0u, 0u);
    TEST_ASSERT_EQUAL_INT(CSI_MODE_ACT_KEEP_STREAMING,
                          csi_mode_tracker_poll(&m, false, &failed_poll));
    TEST_ASSERT_TRUE(m.streaming);
    TEST_ASSERT_EQUAL_UINT32(1280u, m.active_w);
    TEST_ASSERT_EQUAL_UINT32(720u, m.active_h);
    TEST_ASSERT_EQUAL_UINT32(0u, m.stall_recoveries);
    TEST_ASSERT_EQUAL_UINT32(2u, m.sample_errors);

    // NULL sample pointer while streaming also counts as failed sample and keeps streaming.
    TEST_ASSERT_EQUAL_INT(CSI_MODE_ACT_KEEP_STREAMING,
                          csi_mode_tracker_poll(&m, true, NULL));
    TEST_ASSERT_TRUE(m.streaming);
    TEST_ASSERT_EQUAL_UINT32(3u, m.sample_errors);

    // 3. When the next I2C poll succeeds while still stalled (>= 2000 ms without frames),
    //    csi_mode_tracker_poll runs step() -> KEEP_STREAMING -> check_stall() -> RECOVER_STALL!
    csi_mode_sample_t valid_stalled = make_sample(5200u, true, true, true, 1280u, 720u);
    TEST_ASSERT_EQUAL_INT(CSI_MODE_ACT_RECOVER_STALL,
                          csi_mode_tracker_poll(&m, true, &valid_stalled));
    TEST_ASSERT_FALSE(m.streaming);
    TEST_ASSERT_EQUAL_UINT32(1u, m.stall_recoveries);
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
    RUN_TEST(test_mode_tracker_two_sample_stability_and_jitter);
    RUN_TEST(test_mode_tracker_live_resolution_switch_and_signal_loss);
    RUN_TEST(test_mode_tracker_out_of_range_and_abort_start);
    RUN_TEST(test_mode_tracker_hpd_nudge_10_20_40_backoff);
    RUN_TEST(test_mode_tracker_stall_recovery_and_cooldown);
    RUN_TEST(test_mode_tracker_poll_skips_step_and_stall_on_i2c_failure);
    return UNITY_END();
}
