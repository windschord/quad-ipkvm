#include "unity.h"
#include "csi_h264_logic.h"

#include <stdint.h>

void setUp(void) {}
void tearDown(void) {}

// --- 1. Alignment, resolution & buffer sizing ---

static void test_align_up_bounds_and_overflow(void)
{
    TEST_ASSERT_EQUAL_UINT(0u, csi_h264_align_up(100u, 0u));
    TEST_ASSERT_EQUAL_UINT(0u, csi_h264_align_up(0u, 16u));
    TEST_ASSERT_EQUAL_UINT(16u, csi_h264_align_up(1u, 16u));
    TEST_ASSERT_EQUAL_UINT(16u, csi_h264_align_up(16u, 16u));
    TEST_ASSERT_EQUAL_UINT(1088u, csi_h264_align_up(1080u, 16u));
    TEST_ASSERT_EQUAL_UINT(1088u, csi_h264_align_up(1079u, 16u));
    TEST_ASSERT_EQUAL_UINT(1104u, csi_h264_align_up(1089u, 16u));
    TEST_ASSERT_EQUAL_UINT(128u, csi_h264_align_up(128u, 128u));
    TEST_ASSERT_EQUAL_UINT(256u, csi_h264_align_up(129u, 128u));

    // Overflow checks (use non-power-of-2 align so wraparound is non-zero)
    TEST_ASSERT_EQUAL_UINT(0u, csi_h264_align_up(SIZE_MAX, 16u));
    TEST_ASSERT_EQUAL_UINT(0u, csi_h264_align_up(SIZE_MAX - 1u, 16u));
    TEST_ASSERT_EQUAL_UINT(0u, csi_h264_align_up(SIZE_MAX - 2u, 10u));
    // Exact boundary SIZE_MAX == 0 mod 5: val == SIZE_MAX - 1 with align=5 yields SIZE_MAX without overflow.
    TEST_ASSERT_EQUAL_UINT(SIZE_MAX, csi_h264_align_up(SIZE_MAX - 1u, 5u));
    // Largest aligned size_t fits without overflow.
    size_t max_aligned = SIZE_MAX - (SIZE_MAX % 16u);
    TEST_ASSERT_EQUAL_UINT(max_aligned, csi_h264_align_up(max_aligned, 16u));
    TEST_ASSERT_EQUAL_UINT(0u, csi_h264_align_up(max_aligned + 1u, 16u));
}

static void test_check_resolution_boundaries(void)
{
    TEST_ASSERT_TRUE(csi_h264_check_resolution(80u, 80u));
    TEST_ASSERT_TRUE(csi_h264_check_resolution(1280u, 720u));
    TEST_ASSERT_TRUE(csi_h264_check_resolution(1366u, 768u));
    TEST_ASSERT_TRUE(csi_h264_check_resolution(1920u, 1080u));

    // Below minimum
    TEST_ASSERT_FALSE(csi_h264_check_resolution(0u, 720u));
    TEST_ASSERT_FALSE(csi_h264_check_resolution(78u, 80u));
    TEST_ASSERT_FALSE(csi_h264_check_resolution(80u, 78u));
    // Above maximum (P7 contract <= 1920x1080)
    TEST_ASSERT_FALSE(csi_h264_check_resolution(1922u, 1080u));
    TEST_ASSERT_FALSE(csi_h264_check_resolution(1920u, 1082u));
    // Odd dimensions disallowed for YUV420 PPA
    TEST_ASSERT_FALSE(csi_h264_check_resolution(81u, 80u));
    TEST_ASSERT_FALSE(csi_h264_check_resolution(80u, 81u));
    TEST_ASSERT_FALSE(csi_h264_check_resolution(1281u, 720u));
    TEST_ASSERT_FALSE(csi_h264_check_resolution(1280u, 721u));
}

static void test_yuv_and_output_buffer_sizing(void)
{
    // Invalid resolutions yield 0.
    TEST_ASSERT_EQUAL_UINT(0u, csi_h264_calc_yuv_bytes(78u, 80u));
    TEST_ASSERT_EQUAL_UINT(0u, csi_h264_calc_ppa_active_bytes(1920u, 1082u));

    // 720p is already 16-px aligned.
    TEST_ASSERT_EQUAL_UINT(1280u * 720u * 3u / 2u, csi_h264_calc_yuv_bytes(1280u, 720u));
    TEST_ASSERT_EQUAL_UINT(1280u * 720u * 3u / 2u, csi_h264_calc_ppa_active_bytes(1280u, 720u));

    // 1080p requires 16-px padded height 1088 -> 3,133,440 B, active PPA = 3,110,400 B (EVAL §3.2).
    TEST_ASSERT_EQUAL_UINT(3133440u, csi_h264_calc_yuv_bytes(1920u, 1080u));
    TEST_ASSERT_EQUAL_UINT(3110400u, csi_h264_calc_ppa_active_bytes(1920u, 1080u));

    // Width non-multiple-of-16 (1366 -> 1376) keeps aligned stride.
    TEST_ASSERT_EQUAL_UINT(1376u * 768u * 3u / 2u, csi_h264_calc_yuv_bytes(1366u, 768u));
    TEST_ASSERT_EQUAL_UINT(1376u * 768u * 3u / 2u, csi_h264_calc_ppa_active_bytes(1366u, 768u));

    // Any valid yuv size must be a multiple of 128 B for PPA & H.264 cache alignment.
    TEST_ASSERT_EQUAL_UINT(0u, csi_h264_calc_yuv_bytes(1920u, 1080u) % CSI_H264_DMA_ALIGN);
    TEST_ASSERT_EQUAL_UINT(0u, csi_h264_calc_yuv_bytes(1366u, 768u) % CSI_H264_DMA_ALIGN);

    // Output buffer sizing
    TEST_ASSERT_EQUAL_UINT(0u, csi_h264_calc_output_buf_size(0u));
    TEST_ASSERT_EQUAL_UINT(128u, csi_h264_calc_output_buf_size(1u));
    TEST_ASSERT_EQUAL_UINT(128u, csi_h264_calc_output_buf_size(128u));
    TEST_ASSERT_EQUAL_UINT(256u, csi_h264_calc_output_buf_size(129u));
    TEST_ASSERT_EQUAL_UINT(768u * 1024u, csi_h264_calc_output_buf_size(768u * 1024u));
    TEST_ASSERT_EQUAL_UINT(CSI_H264_MAX_OUT_BYTES,
                           csi_h264_calc_output_buf_size(CSI_H264_MAX_OUT_BYTES));
    TEST_ASSERT_EQUAL_UINT(0u, csi_h264_calc_output_buf_size(CSI_H264_MAX_OUT_BYTES + 1u));
}

static void test_target_fps_and_param_validation(void)
{
    TEST_ASSERT_EQUAL_UINT(0u, csi_h264_calc_target_fps(64u, 64u));
    TEST_ASSERT_EQUAL_UINT(20u, csi_h264_calc_target_fps(640u, 480u));
    TEST_ASSERT_EQUAL_UINT(20u, csi_h264_calc_target_fps(1280u, 720u));
    TEST_ASSERT_EQUAL_UINT(9u, csi_h264_calc_target_fps(1282u, 720u));
    TEST_ASSERT_EQUAL_UINT(9u, csi_h264_calc_target_fps(1280u, 722u));
    TEST_ASSERT_EQUAL_UINT(9u, csi_h264_calc_target_fps(1920u, 1080u));

    TEST_ASSERT_TRUE(csi_h264_validate_enc_params(1280u, 720u, 1u, 1u, 1u, 0u, 0u));
    TEST_ASSERT_TRUE(csi_h264_validate_enc_params(1920u, 1080u, 255u, 30u, 4000000u, 51u, 51u));
    TEST_ASSERT_FALSE(csi_h264_validate_enc_params(64u, 480u, 30u, 20u, 1500000u, 20u, 40u));
    TEST_ASSERT_FALSE(csi_h264_validate_enc_params(1280u, 720u, 0u, 20u, 1500000u, 20u, 40u));
    TEST_ASSERT_FALSE(csi_h264_validate_enc_params(1280u, 720u, 30u, 0u, 1500000u, 20u, 40u));
    TEST_ASSERT_FALSE(csi_h264_validate_enc_params(1280u, 720u, 30u, 20u, 0u, 20u, 40u));
    TEST_ASSERT_FALSE(csi_h264_validate_enc_params(1280u, 720u, 30u, 20u, 1500000u, 41u, 40u));
    TEST_ASSERT_FALSE(csi_h264_validate_enc_params(1280u, 720u, 30u, 20u, 1500000u, 20u, 52u));
}

// --- 2. Annex B bitstream validation ---

static void test_check_frame_empty_and_start_code(void)
{
    const uint8_t sps_idr[6] = {0x00, 0x00, 0x00, 0x01, 0x67, 0x42};

    TEST_ASSERT_EQUAL_INT(CSI_H264_FRAME_ERR_EMPTY, csi_h264_check_frame(NULL, 6u, true, 1024u));
    TEST_ASSERT_EQUAL_INT(CSI_H264_FRAME_ERR_EMPTY, csi_h264_check_frame(sps_idr, 0u, true, 1024u));
    TEST_ASSERT_EQUAL_INT(CSI_H264_FRAME_ERR_EMPTY, csi_h264_check_frame(sps_idr, 4u, true, 1024u));
    TEST_ASSERT_EQUAL_INT(CSI_H264_FRAME_OK, csi_h264_check_frame(sps_idr, 5u, true, 5u));

    uint8_t bad[6];
    for (int i = 0; i < 4; i++) {
        for (size_t j = 0; j < sizeof(bad); j++) {
            bad[j] = sps_idr[j];
        }
        bad[i] ^= 0x01u;
        TEST_ASSERT_EQUAL_INT(CSI_H264_FRAME_ERR_FORMAT, csi_h264_check_frame(bad, 6u, true, 1024u));
    }

    // forbidden_zero_bit set in NAL header
    const uint8_t bad_forbidden[5] = {0x00, 0x00, 0x00, 0x01, 0xE7};
    TEST_ASSERT_EQUAL_INT(CSI_H264_FRAME_ERR_FORMAT,
                          csi_h264_check_frame(bad_forbidden, sizeof(bad_forbidden), false, 1024u));
}

static void test_check_frame_key_and_oversize(void)
{
    const uint8_t sps[5] = {0x00, 0x00, 0x00, 0x01, 0x67}; // NAL type 7 (SPS)
    const uint8_t idr[5] = {0x00, 0x00, 0x00, 0x01, 0x65}; // NAL type 5 (IDR)
    const uint8_t p_slc[5] = {0x00, 0x00, 0x00, 0x01, 0x41}; // NAL type 1 (non-IDR slice)
    const uint8_t p_slc_noref[5] = {0x00, 0x00, 0x00, 0x01, 0x01};
    const uint8_t pps_only[5] = {0x00, 0x00, 0x00, 0x01, 0x68}; // NAL type 8 (PPS)

    TEST_ASSERT_EQUAL_INT(CSI_H264_FRAME_OK, csi_h264_check_frame(sps, 5u, true, 1024u));
    TEST_ASSERT_EQUAL_INT(CSI_H264_FRAME_OK, csi_h264_check_frame(idr, 5u, true, 1024u));
    TEST_ASSERT_EQUAL_INT(CSI_H264_FRAME_ERR_FORMAT, csi_h264_check_frame(p_slc, 5u, true, 1024u));
    TEST_ASSERT_EQUAL_INT(CSI_H264_FRAME_ERR_FORMAT, csi_h264_check_frame(pps_only, 5u, true, 1024u));

    TEST_ASSERT_EQUAL_INT(CSI_H264_FRAME_OK, csi_h264_check_frame(p_slc, 5u, false, 1024u));
    TEST_ASSERT_EQUAL_INT(CSI_H264_FRAME_OK, csi_h264_check_frame(p_slc_noref, 5u, false, 1024u));

    // Exact bound vs oversize
    TEST_ASSERT_EQUAL_INT(CSI_H264_FRAME_OK, csi_h264_check_frame(p_slc, 5u, false, 5u));
    TEST_ASSERT_EQUAL_INT(CSI_H264_FRAME_ERR_OVERSIZE, csi_h264_check_frame(p_slc, 5u, false, 4u));
    TEST_ASSERT_EQUAL_INT(CSI_H264_FRAME_ERR_OVERSIZE, csi_h264_check_frame(sps, 5u, true, 0u));
}

// --- 3. Coordinator state machine (set_codec, request_keyframe, GOP, self-IDR recovery) ---

static void test_ctrl_null_and_incapable(void)
{
    bool use_h264 = true;
    bool force_idr = true;

    csi_h264_ctrl_init(NULL, true, 30u);
    TEST_ASSERT_EQUAL_INT(-1, csi_h264_ctrl_set_codec(NULL, CSI_H264_CODEC_MJPEG));
    csi_h264_ctrl_request_keyframe(NULL);
    csi_h264_ctrl_on_mode_reset(NULL);
    TEST_ASSERT_FALSE(csi_h264_ctrl_begin_frame(NULL, 1280u, 720u, &use_h264, &force_idr));
    TEST_ASSERT_FALSE(use_h264);
    TEST_ASSERT_FALSE(force_idr);
    csi_h264_ctrl_end_frame(NULL, CSI_H264_OUTCOME_PUBLISHED, true);
    TEST_ASSERT_EQUAL_INT(CSI_H264_CODEC_MJPEG, csi_h264_ctrl_get_requested_codec(NULL));
    TEST_ASSERT_EQUAL_INT(CSI_H264_CODEC_MJPEG, csi_h264_ctrl_get_active_codec(NULL));

    csi_h264_ctrl_t c;
    csi_h264_ctrl_init(&c, false, 0u);
    TEST_ASSERT_EQUAL_UINT(CSI_H264_MIN_GOP, c.gop);
    TEST_ASSERT_EQUAL_INT(0, csi_h264_ctrl_set_codec(&c, CSI_H264_CODEC_MJPEG));
    TEST_ASSERT_EQUAL_INT(-1, csi_h264_ctrl_set_codec(&c, CSI_H264_CODEC_H264));
    TEST_ASSERT_EQUAL_INT(-1, csi_h264_ctrl_set_codec(&c, (csi_h264_codec_t)99));
    csi_h264_ctrl_request_keyframe(&c);
    TEST_ASSERT_FALSE(csi_h264_ctrl_begin_frame(&c, 1280u, 720u, &use_h264, &force_idr));
    TEST_ASSERT_FALSE(use_h264);
    TEST_ASSERT_FALSE(force_idr);
}

static void test_ctrl_mjpeg_request_keyframe_is_noop(void)
{
    csi_h264_ctrl_t c;
    bool use_h264 = false;
    bool force_idr = false;
    csi_h264_ctrl_init(&c, true, 30u);

    // Stay in MJPEG; request_keyframe must do nothing (contract §9.7).
    csi_h264_ctrl_request_keyframe(&c);
    TEST_ASSERT_EQUAL_UINT(0u, c.keyframe_req);
    TEST_ASSERT_FALSE(csi_h264_ctrl_begin_frame(&c, 1280u, 720u, &use_h264, &force_idr));
    TEST_ASSERT_FALSE(use_h264);
    TEST_ASSERT_FALSE(force_idr);
    TEST_ASSERT_EQUAL_UINT(0u, c.keyframe_reqs_applied);

    // Even when requesting H.264 before begin_frame runs (active is still MJPEG),
    // request_keyframe stays a no-op and the switch itself supplies the initial IDR.
    TEST_ASSERT_EQUAL_INT(0, csi_h264_ctrl_set_codec(&c, CSI_H264_CODEC_H264));
    TEST_ASSERT_EQUAL_INT(CSI_H264_CODEC_H264, csi_h264_ctrl_get_requested_codec(&c));
    TEST_ASSERT_EQUAL_INT(CSI_H264_CODEC_MJPEG, csi_h264_ctrl_get_active_codec(&c));
    csi_h264_ctrl_request_keyframe(&c);
    TEST_ASSERT_EQUAL_UINT(0u, c.keyframe_req);

    TEST_ASSERT_TRUE(csi_h264_ctrl_begin_frame(&c, 1280u, 720u, &use_h264, &force_idr));
    TEST_ASSERT_TRUE(use_h264);
    TEST_ASSERT_TRUE(force_idr);
    TEST_ASSERT_EQUAL_INT(CSI_H264_CODEC_H264, csi_h264_ctrl_get_active_codec(&c));
    TEST_ASSERT_EQUAL_UINT(0u, c.keyframe_reqs_applied);
    TEST_ASSERT_EQUAL_UINT(1u, c.switches_to_h264);
}

static void test_ctrl_switch_gop_and_request_keyframe(void)
{
    csi_h264_ctrl_t c;
    bool use_h264 = false;
    bool force_idr = false;
    csi_h264_ctrl_init(&c, true, 3u);

    TEST_ASSERT_EQUAL_INT(0, csi_h264_ctrl_set_codec(&c, CSI_H264_CODEC_H264));

    // Invalid codec enum still rejected cleanly.
    TEST_ASSERT_EQUAL_INT(-1, csi_h264_ctrl_set_codec(&c, (csi_h264_codec_t)2));

    // Frame 0: switch -> IDR required.
    TEST_ASSERT_TRUE(csi_h264_ctrl_begin_frame(&c, 1280u, 720u, &use_h264, &force_idr));
    TEST_ASSERT_TRUE(use_h264);
    TEST_ASSERT_TRUE(force_idr);
    csi_h264_ctrl_end_frame(&c, CSI_H264_OUTCOME_PUBLISHED, true);
    TEST_ASSERT_EQUAL_UINT(1u, c.idr_frames);

    // Frame 1: normal P frame.
    TEST_ASSERT_FALSE(csi_h264_ctrl_begin_frame(&c, 1280u, 720u, &use_h264, &force_idr));
    TEST_ASSERT_TRUE(use_h264);
    TEST_ASSERT_FALSE(force_idr);
    csi_h264_ctrl_end_frame(&c, CSI_H264_OUTCOME_PUBLISHED, false);

    // Frame 2: normal P frame.
    TEST_ASSERT_FALSE(csi_h264_ctrl_begin_frame(&c, 1280u, 720u, &use_h264, &force_idr));
    TEST_ASSERT_FALSE(force_idr);
    csi_h264_ctrl_end_frame(&c, CSI_H264_OUTCOME_PUBLISHED, false);

    // Frame 3: periodic GOP=3 hit -> IDR forced.
    TEST_ASSERT_FALSE(csi_h264_ctrl_begin_frame(&c, 1280u, 720u, &use_h264, &force_idr));
    TEST_ASSERT_TRUE(force_idr);
    csi_h264_ctrl_end_frame(&c, CSI_H264_OUTCOME_PUBLISHED, true);
    TEST_ASSERT_EQUAL_UINT(2u, c.idr_frames);
    TEST_ASSERT_EQUAL_UINT(2u, c.p_frames);

    // Multiple external keyframe_requests merge into one applied IDR.
    csi_h264_ctrl_request_keyframe(&c);
    csi_h264_ctrl_request_keyframe(&c);
    TEST_ASSERT_FALSE(csi_h264_ctrl_begin_frame(&c, 1280u, 720u, &use_h264, &force_idr));
    TEST_ASSERT_TRUE(force_idr);
    TEST_ASSERT_EQUAL_UINT(1u, c.keyframe_reqs_applied);
    csi_h264_ctrl_end_frame(&c, CSI_H264_OUTCOME_PUBLISHED, true);

    // Next frame resumes P.
    TEST_ASSERT_FALSE(csi_h264_ctrl_begin_frame(&c, 1280u, 720u, &use_h264, &force_idr));
    TEST_ASSERT_FALSE(force_idr);
}

static void test_ctrl_self_idr_recovery_and_fallback(void)
{
    csi_h264_ctrl_t c;
    bool use_h264 = false;
    bool force_idr = false;
    csi_h264_ctrl_init(&c, true, 30u);
    TEST_ASSERT_EQUAL_INT(0, csi_h264_ctrl_set_codec(&c, CSI_H264_CODEC_H264));

    // If first IDR hits PUBLISH_BUSY, need_idr stays latched for next frame!
    TEST_ASSERT_TRUE(csi_h264_ctrl_begin_frame(&c, 1920u, 1080u, &use_h264, &force_idr));
    TEST_ASSERT_TRUE(force_idr);
    csi_h264_ctrl_end_frame(&c, CSI_H264_OUTCOME_PUBLISH_BUSY, true);
    TEST_ASSERT_EQUAL_UINT(1u, c.self_idr_recoveries);

    TEST_ASSERT_FALSE(csi_h264_ctrl_begin_frame(&c, 1920u, 1080u, &use_h264, &force_idr));
    TEST_ASSERT_TRUE(force_idr);
    csi_h264_ctrl_end_frame(&c, CSI_H264_OUTCOME_PUBLISHED, true);

    // Subsequent P hits OVERSIZE -> next frame forced IDR.
    TEST_ASSERT_FALSE(csi_h264_ctrl_begin_frame(&c, 1920u, 1080u, &use_h264, &force_idr));
    TEST_ASSERT_FALSE(force_idr);
    csi_h264_ctrl_end_frame(&c, CSI_H264_OUTCOME_OVERSIZE, false);
    TEST_ASSERT_EQUAL_UINT(2u, c.self_idr_recoveries);

    TEST_ASSERT_FALSE(csi_h264_ctrl_begin_frame(&c, 1920u, 1080u, &use_h264, &force_idr));
    TEST_ASSERT_TRUE(force_idr);
    csi_h264_ctrl_end_frame(&c, CSI_H264_OUTCOME_ENCODE_ERR, false);
    TEST_ASSERT_EQUAL_UINT(3u, c.self_idr_recoveries);

    // Next frame still forces IDR after encode error.
    TEST_ASSERT_FALSE(csi_h264_ctrl_begin_frame(&c, 1920u, 1080u, &use_h264, &force_idr));
    TEST_ASSERT_TRUE(force_idr);
    // Suppose HW unexpectedly returned P instead of IDR: need_idr remains latched and frames_since_idr stays 0.
    csi_h264_ctrl_end_frame(&c, CSI_H264_OUTCOME_PUBLISHED, false);
    TEST_ASSERT_EQUAL_UINT(0u, c.frames_since_idr);
    TEST_ASSERT_FALSE(csi_h264_ctrl_begin_frame(&c, 1920u, 1080u, &use_h264, &force_idr));
    TEST_ASSERT_TRUE(force_idr);
    csi_h264_ctrl_end_frame(&c, CSI_H264_OUTCOME_PUBLISHED, true);

    // MJPEG fallback transitions active_codec -> MJPEG with OOM cooldown
    // without incrementing self_idr_recoveries.
    csi_h264_ctrl_end_frame(&c, CSI_H264_OUTCOME_FALLBACK_MJPEG, false);
    TEST_ASSERT_EQUAL_UINT(3u, c.self_idr_recoveries);
    TEST_ASSERT_EQUAL_INT(CSI_H264_CODEC_MJPEG, csi_h264_ctrl_get_active_codec(&c));
    TEST_ASSERT_FALSE(csi_h264_ctrl_begin_frame(&c, 1920u, 1080u, &use_h264, &force_idr));
    TEST_ASSERT_FALSE(use_h264);
    TEST_ASSERT_FALSE(force_idr);
    csi_h264_ctrl_on_mode_reset(&c);
    TEST_ASSERT_TRUE(csi_h264_ctrl_begin_frame(&c, 1920u, 1080u, &use_h264, &force_idr));
    TEST_ASSERT_TRUE(use_h264);
    TEST_ASSERT_TRUE(force_idr);
    csi_h264_ctrl_end_frame(&c, CSI_H264_OUTCOME_PUBLISHED, true);

    // Resolution change (including single-axis changes) forces IDR even mid-GOP.
    TEST_ASSERT_FALSE(csi_h264_ctrl_begin_frame(&c, 1280u, 720u, &use_h264, &force_idr));
    TEST_ASSERT_TRUE(force_idr);
    csi_h264_ctrl_end_frame(&c, CSI_H264_OUTCOME_PUBLISHED, true);

    TEST_ASSERT_FALSE(csi_h264_ctrl_begin_frame(&c, 1280u, 800u, &use_h264, &force_idr));
    TEST_ASSERT_TRUE(force_idr);
    csi_h264_ctrl_end_frame(&c, CSI_H264_OUTCOME_PUBLISHED, true);

    TEST_ASSERT_FALSE(csi_h264_ctrl_begin_frame(&c, 1440u, 800u, &use_h264, &force_idr));
    TEST_ASSERT_TRUE(force_idr);
    csi_h264_ctrl_end_frame(&c, CSI_H264_OUTCOME_PUBLISHED, true);

    // Mode reset forces IDR even at the same resolution.
    csi_h264_ctrl_on_mode_reset(&c);
    TEST_ASSERT_FALSE(csi_h264_ctrl_begin_frame(&c, 1280u, 720u, &use_h264, &force_idr));
    TEST_ASSERT_TRUE(force_idr);
    csi_h264_ctrl_end_frame(&c, CSI_H264_OUTCOME_PUBLISHED, true);

    // Out-of-range resolution temporarily disables H.264.
    csi_h264_ctrl_request_keyframe(&c);
    TEST_ASSERT_TRUE(csi_h264_ctrl_begin_frame(&c, 1922u, 1080u, &use_h264, &force_idr));
    TEST_ASSERT_FALSE(use_h264);
    TEST_ASSERT_FALSE(force_idr);
    TEST_ASSERT_EQUAL_UINT(2u, c.switches_to_mjpeg);
    TEST_ASSERT_EQUAL_INT(CSI_H264_CODEC_MJPEG, csi_h264_ctrl_get_active_codec(&c));

    // Returning to valid resolution switches cleanly back to H.264 with IDR forced.
    TEST_ASSERT_TRUE(csi_h264_ctrl_begin_frame(&c, 1280u, 720u, &use_h264, &force_idr));
    TEST_ASSERT_TRUE(use_h264);
    TEST_ASSERT_TRUE(force_idr);
    TEST_ASSERT_EQUAL_UINT(3u, c.switches_to_h264);
}

static void test_ctrl_downgrade_clears_pending_request(void)
{
    csi_h264_ctrl_t c;
    bool use_h264 = false;
    bool force_idr = false;
    csi_h264_ctrl_init(&c, true, 30u);
    TEST_ASSERT_EQUAL_INT(0, csi_h264_ctrl_set_codec(&c, CSI_H264_CODEC_H264));

    TEST_ASSERT_TRUE(csi_h264_ctrl_begin_frame(&c, 1280u, 720u, &use_h264, &force_idr));
    csi_h264_ctrl_end_frame(&c, CSI_H264_OUTCOME_PUBLISHED, true);

    // Keyframe requested right before downgrade to MJPEG
    csi_h264_ctrl_request_keyframe(&c);
    TEST_ASSERT_EQUAL_INT(0, csi_h264_ctrl_set_codec(&c, CSI_H264_CODEC_MJPEG));

    TEST_ASSERT_TRUE(csi_h264_ctrl_begin_frame(&c, 1280u, 720u, &use_h264, &force_idr));
    TEST_ASSERT_FALSE(use_h264);
    TEST_ASSERT_FALSE(force_idr);
    TEST_ASSERT_EQUAL_UINT(0u, c.keyframe_req);
    TEST_ASSERT_EQUAL_UINT(0u, c.keyframe_reqs_applied);
}

static uint32_t next_prng(uint32_t *s)
{
    uint32_t x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x;
    return x;
}

static void test_ctrl_fuzz_invariants(void)
{
    csi_h264_ctrl_t c;
    const uint8_t gop = 5u;
    csi_h264_ctrl_init(&c, true, gop);

    uint32_t seed = 0x13572468u;
    const uint32_t widths[3] = {1280u, 1920u, 1922u};
    const uint32_t heights[3] = {720u, 1080u, 1080u};

    bool expect_idr_next = false;
    uint32_t consec_p = 0u;
    csi_h264_codec_t prev_active = CSI_H264_CODEC_MJPEG;

    for (int step = 0; step < 2000; step++) {
        uint32_t r = next_prng(&seed);
        if ((r % 5u) == 0u) {
            (void)csi_h264_ctrl_set_codec(
                &c, (r & 0x10u) ? CSI_H264_CODEC_H264 : CSI_H264_CODEC_MJPEG);
        }
        if ((r % 7u) == 0u) {
            bool was_h264_active = (csi_h264_ctrl_get_active_codec(&c) == CSI_H264_CODEC_H264);
            csi_h264_ctrl_request_keyframe(&c);
            if (!was_h264_active) {
                TEST_ASSERT_EQUAL_UINT(0u, c.keyframe_req);
            } else {
                expect_idr_next = true;
            }
        }
        if ((r % 19u) == 0u) {
            csi_h264_ctrl_on_mode_reset(&c);
            expect_idr_next = true;
            consec_p = 0u;
        }

        uint32_t idx = (r >> 8) % 3u;
        uint32_t w = widths[idx];
        uint32_t h = heights[idx];
        bool mode_changed = (w != c.active_w || h != c.active_h);

        bool use_h264 = false;
        bool force_idr = false;
        csi_h264_ctrl_begin_frame(&c, w, h, &use_h264, &force_idr);

        if (!use_h264) {
            TEST_ASSERT_FALSE(force_idr);
            TEST_ASSERT_EQUAL_INT(CSI_H264_CODEC_MJPEG, csi_h264_ctrl_get_active_codec(&c));
            prev_active = CSI_H264_CODEC_MJPEG;
            expect_idr_next = true;
            consec_p = 0u;
            continue;
        }

        TEST_ASSERT_EQUAL_INT(CSI_H264_CODEC_H264, csi_h264_ctrl_get_active_codec(&c));
        if (prev_active == CSI_H264_CODEC_MJPEG || mode_changed) {
            expect_idr_next = true;
        }
        if (expect_idr_next || consec_p + 1u >= gop) {
            TEST_ASSERT_TRUE(force_idr);
        }
        prev_active = CSI_H264_CODEC_H264;

        uint32_t out_sel = (r >> 12) % 6u;
        csi_h264_frame_outcome_t outcome = CSI_H264_OUTCOME_PUBLISHED;
        if (out_sel == 3u) {
            outcome = CSI_H264_OUTCOME_PUBLISH_BUSY;
        } else if (out_sel == 4u) {
            outcome = CSI_H264_OUTCOME_OVERSIZE;
        } else if (out_sel == 5u) {
            outcome = CSI_H264_OUTCOME_ENCODE_ERR;
        }

        bool is_idr = (outcome == CSI_H264_OUTCOME_PUBLISHED) ? force_idr : false;
        csi_h264_ctrl_end_frame(&c, outcome, is_idr);

        if (outcome == CSI_H264_OUTCOME_PUBLISHED) {
            if (is_idr) {
                consec_p = 0u;
                expect_idr_next = false;
            } else {
                consec_p++;
                TEST_ASSERT_LESS_THAN_UINT(gop, consec_p);
            }
        } else {
            expect_idr_next = true;
            consec_p = 0u;
        }
    }
}

static void test_ctrl_oom_fallback_and_retry(void)
{
    csi_h264_ctrl_t c;
    csi_h264_ctrl_init(&c, true, 10u);
    csi_h264_ctrl_set_codec(&c, CSI_H264_CODEC_H264);

    bool use_h264 = false;
    bool force_idr = false;
    TEST_ASSERT_TRUE(csi_h264_ctrl_begin_frame(&c, 1280u, 720u, &use_h264, &force_idr));
    TEST_ASSERT_TRUE(use_h264);
    TEST_ASSERT_TRUE(force_idr);
    TEST_ASSERT_EQUAL_UINT(1u, c.switches_to_h264);

    // Pending keyframe request must be discarded immediately on OOM fallback to MJPEG
    // (must not leak across an immediate mode reset before the next begin_frame).
    csi_h264_ctrl_request_keyframe(&c);
    csi_h264_ctrl_end_frame(&c, CSI_H264_OUTCOME_FALLBACK_MJPEG, false);
    TEST_ASSERT_EQUAL_UINT(0u, c.keyframe_req);
    TEST_ASSERT_EQUAL_INT(CSI_H264_CODEC_MJPEG, csi_h264_ctrl_get_active_codec(&c));
    TEST_ASSERT_EQUAL_UINT(1u, c.switches_to_mjpeg);
    TEST_ASSERT_EQUAL_UINT(0u, c.self_idr_recoveries);
    TEST_ASSERT_EQUAL_UINT(CSI_H264_OOM_RETRY_FRAMES, c.oom_cooldown_frames);

    // Verify immediate mode reset after FALLBACK_MJPEG does not apply a stale keyframe request.
    csi_h264_ctrl_on_mode_reset(&c);
    TEST_ASSERT_TRUE(csi_h264_ctrl_begin_frame(&c, 1280u, 720u, &use_h264, &force_idr));
    TEST_ASSERT_TRUE(use_h264);
    TEST_ASSERT_TRUE(force_idr);
    TEST_ASSERT_EQUAL_UINT(0u, c.keyframe_reqs_applied);
    csi_h264_ctrl_request_keyframe(&c);
    csi_h264_ctrl_end_frame(&c, CSI_H264_OUTCOME_FALLBACK_MJPEG, false);
    TEST_ASSERT_EQUAL_UINT(2u, c.switches_to_mjpeg);
    TEST_ASSERT_EQUAL_UINT(2u, c.switches_to_h264);

    // Idempotent second FALLBACK_MJPEG must not double-count switches_to_mjpeg.
    csi_h264_ctrl_end_frame(&c, CSI_H264_OUTCOME_FALLBACK_MJPEG, false);
    TEST_ASSERT_EQUAL_UINT(2u, c.switches_to_mjpeg);

    // Cooldown runs for exactly CSI_H264_OOM_RETRY_FRAMES MJPEG frames, keeping requested=H264.
    for (uint32_t i = 0u; i < CSI_H264_OOM_RETRY_FRAMES; i++) {
        if (i == 5u) {
            csi_h264_ctrl_request_keyframe(&c); // no-op while active_codec == MJPEG
        }
        TEST_ASSERT_FALSE(csi_h264_ctrl_begin_frame(&c, 1280u, 720u, &use_h264, &force_idr));
        TEST_ASSERT_FALSE(use_h264);
        TEST_ASSERT_FALSE(force_idr);
        TEST_ASSERT_EQUAL_INT(CSI_H264_CODEC_MJPEG, csi_h264_ctrl_get_active_codec(&c));
        TEST_ASSERT_EQUAL_INT(CSI_H264_CODEC_H264, csi_h264_ctrl_get_requested_codec(&c));
    }
    TEST_ASSERT_EQUAL_UINT(0u, c.oom_cooldown_frames);

    // Next frame after cooldown automatically retries H.264 with forced IDR.
    TEST_ASSERT_TRUE(csi_h264_ctrl_begin_frame(&c, 1280u, 720u, &use_h264, &force_idr));
    TEST_ASSERT_TRUE(use_h264);
    TEST_ASSERT_TRUE(force_idr);
    TEST_ASSERT_EQUAL_UINT(3u, c.switches_to_h264);
    TEST_ASSERT_EQUAL_UINT(0u, c.keyframe_reqs_applied);
    csi_h264_ctrl_end_frame(&c, CSI_H264_OUTCOME_PUBLISHED, true);

    // Re-requesting H.264 during cooldown clears cooldown immediately via requested_epoch.
    csi_h264_ctrl_end_frame(&c, CSI_H264_OUTCOME_FALLBACK_MJPEG, false);
    TEST_ASSERT_FALSE(csi_h264_ctrl_begin_frame(&c, 1280u, 720u, &use_h264, &force_idr));
    TEST_ASSERT_FALSE(use_h264);
    csi_h264_ctrl_set_codec(&c, CSI_H264_CODEC_H264);
    TEST_ASSERT_TRUE(csi_h264_ctrl_begin_frame(&c, 1280u, 720u, &use_h264, &force_idr));
    TEST_ASSERT_TRUE(use_h264);
    TEST_ASSERT_TRUE(force_idr);
    csi_h264_ctrl_end_frame(&c, CSI_H264_OUTCOME_PUBLISHED, true);

    // Explicit switch to MJPEG during cooldown clears cooldown before any later switch back.
    csi_h264_ctrl_end_frame(&c, CSI_H264_OUTCOME_FALLBACK_MJPEG, false);
    csi_h264_ctrl_set_codec(&c, CSI_H264_CODEC_MJPEG);
    TEST_ASSERT_FALSE(csi_h264_ctrl_begin_frame(&c, 1280u, 720u, &use_h264, &force_idr));
    TEST_ASSERT_FALSE(use_h264);
    TEST_ASSERT_EQUAL_UINT(0u, c.oom_cooldown_frames);
    csi_h264_ctrl_set_codec(&c, CSI_H264_CODEC_H264);
    TEST_ASSERT_TRUE(csi_h264_ctrl_begin_frame(&c, 1280u, 720u, &use_h264, &force_idr));
    TEST_ASSERT_TRUE(use_h264);
    TEST_ASSERT_TRUE(force_idr);
    csi_h264_ctrl_end_frame(&c, CSI_H264_OUTCOME_PUBLISHED, true);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_align_up_bounds_and_overflow);
    RUN_TEST(test_check_resolution_boundaries);
    RUN_TEST(test_yuv_and_output_buffer_sizing);
    RUN_TEST(test_target_fps_and_param_validation);
    RUN_TEST(test_check_frame_empty_and_start_code);
    RUN_TEST(test_check_frame_key_and_oversize);
    RUN_TEST(test_ctrl_null_and_incapable);
    RUN_TEST(test_ctrl_mjpeg_request_keyframe_is_noop);
    RUN_TEST(test_ctrl_switch_gop_and_request_keyframe);
    RUN_TEST(test_ctrl_self_idr_recovery_and_fallback);
    RUN_TEST(test_ctrl_downgrade_clears_pending_request);
    RUN_TEST(test_ctrl_oom_fallback_and_retry);
    RUN_TEST(test_ctrl_fuzz_invariants);
    return UNITY_END();
}
