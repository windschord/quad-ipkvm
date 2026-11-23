#include "unity.h"
#include "csi_h264_logic.h"

#include <stdint.h>
#include <string.h>

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
    TEST_ASSERT_TRUE(csi_h264_check_resolution(1280u, 722u));
    TEST_ASSERT_TRUE(csi_h264_check_resolution(1360u, 768u));
    TEST_ASSERT_TRUE(csi_h264_check_resolution(1920u, 1080u));

    // Non-16-aligned width rejected until HW-verified (PR #128 review #1)
    TEST_ASSERT_FALSE(csi_h264_check_resolution(1366u, 768u));
    TEST_ASSERT_FALSE(csi_h264_check_resolution(1282u, 720u));
    TEST_ASSERT_FALSE(csi_h264_check_resolution(82u, 80u));

    // Below minimum
    TEST_ASSERT_FALSE(csi_h264_check_resolution(0u, 720u));
    TEST_ASSERT_FALSE(csi_h264_check_resolution(64u, 80u));
    TEST_ASSERT_FALSE(csi_h264_check_resolution(80u, 78u));
    // Above maximum (P7 contract <= 1920x1080)
    TEST_ASSERT_FALSE(csi_h264_check_resolution(1936u, 1080u));
    TEST_ASSERT_FALSE(csi_h264_check_resolution(1920u, 1082u));
    // Odd dimensions disallowed
    TEST_ASSERT_FALSE(csi_h264_check_resolution(81u, 80u));
    TEST_ASSERT_FALSE(csi_h264_check_resolution(80u, 81u));
    TEST_ASSERT_FALSE(csi_h264_check_resolution(1281u, 720u));
    TEST_ASSERT_FALSE(csi_h264_check_resolution(1280u, 721u));
}

static void test_yuv_and_output_buffer_sizing(void)
{
    // Invalid resolutions (including non-16-aligned widths) yield 0.
    TEST_ASSERT_EQUAL_UINT(0u, csi_h264_calc_yuv_bytes(64u, 80u));
    TEST_ASSERT_EQUAL_UINT(0u, csi_h264_calc_yuv_bytes(1366u, 768u));
    TEST_ASSERT_EQUAL_UINT(0u, csi_h264_calc_ppa_active_bytes(1920u, 1082u));

    // 720p is already 16-px aligned.
    TEST_ASSERT_EQUAL_UINT(1280u * 720u * 3u / 2u, csi_h264_calc_yuv_bytes(1280u, 720u));
    TEST_ASSERT_EQUAL_UINT(1280u * 720u * 3u / 2u, csi_h264_calc_ppa_active_bytes(1280u, 720u));

    // 1080p requires 16-px padded height 1088 -> 3,133,440 B, active PPA = 3,110,400 B (EVAL §3.2).
    TEST_ASSERT_EQUAL_UINT(3133440u, csi_h264_calc_yuv_bytes(1920u, 1080u));
    TEST_ASSERT_EQUAL_UINT(3110400u, csi_h264_calc_ppa_active_bytes(1920u, 1080u));

    // Non-16-aligned even height (1280x722) pads height to 736 while active PPA uses 722.
    TEST_ASSERT_EQUAL_UINT(1280u * 736u * 3u / 2u, csi_h264_calc_yuv_bytes(1280u, 722u));
    TEST_ASSERT_EQUAL_UINT(1280u * 722u * 3u / 2u, csi_h264_calc_ppa_active_bytes(1280u, 722u));

    // Any valid yuv size must be a multiple of 128 B for PPA & H.264 cache alignment.
    TEST_ASSERT_EQUAL_UINT(0u, csi_h264_calc_yuv_bytes(1920u, 1080u) % CSI_H264_DMA_ALIGN);
    TEST_ASSERT_EQUAL_UINT(0u, csi_h264_calc_yuv_bytes(1280u, 722u) % CSI_H264_DMA_ALIGN);

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
    TEST_ASSERT_EQUAL_UINT(0u, csi_h264_calc_target_fps(1282u, 720u));
    TEST_ASSERT_EQUAL_UINT(20u, csi_h264_calc_target_fps(640u, 480u));
    TEST_ASSERT_EQUAL_UINT(20u, csi_h264_calc_target_fps(1280u, 720u));
    TEST_ASSERT_EQUAL_UINT(20u, csi_h264_calc_target_fps(1296u, 720u));
    TEST_ASSERT_EQUAL_UINT(20u, csi_h264_calc_target_fps(1280u, 722u));
    TEST_ASSERT_EQUAL_UINT(14u, csi_h264_calc_target_fps(1440u, 900u));
    TEST_ASSERT_EQUAL_UINT(13u, csi_h264_calc_target_fps(1600u, 900u));
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
    const uint8_t sps_idr[11] = {
        0x00, 0x00, 0x00, 0x01, 0x67, 0x42,
        0x00, 0x00, 0x00, 0x01, 0x65,
    };

    TEST_ASSERT_EQUAL_INT(CSI_H264_FRAME_ERR_EMPTY, csi_h264_check_frame(NULL, 11u, true, 1024u));
    TEST_ASSERT_EQUAL_INT(CSI_H264_FRAME_ERR_EMPTY, csi_h264_check_frame(sps_idr, 0u, true, 1024u));
    TEST_ASSERT_EQUAL_INT(CSI_H264_FRAME_ERR_EMPTY, csi_h264_check_frame(sps_idr, 4u, true, 1024u));
    TEST_ASSERT_EQUAL_INT(CSI_H264_FRAME_ERR_FORMAT, csi_h264_check_frame(sps_idr, 5u, true, 1024u));
    TEST_ASSERT_EQUAL_INT(CSI_H264_FRAME_OK, csi_h264_check_frame(sps_idr, sizeof(sps_idr), true, sizeof(sps_idr)));

    uint8_t bad[11];
    for (int i = 0; i < 4; i++) {
        for (size_t j = 0; j < sizeof(bad); j++) {
            bad[j] = sps_idr[j];
        }
        bad[i] ^= 0x01u;
        TEST_ASSERT_EQUAL_INT(CSI_H264_FRAME_ERR_FORMAT, csi_h264_check_frame(bad, sizeof(bad), true, 1024u));
    }

    // forbidden_zero_bit set in leading NAL header
    const uint8_t bad_forbidden[5] = {0x00, 0x00, 0x00, 0x01, 0xE7};
    TEST_ASSERT_EQUAL_INT(CSI_H264_FRAME_ERR_FORMAT,
                          csi_h264_check_frame(bad_forbidden, sizeof(bad_forbidden), false, 1024u));
}

static void test_check_frame_key_and_oversize(void)
{
    const uint8_t sps_only[5] = {0x00, 0x00, 0x00, 0x01, 0x67}; // NAL type 7 only
    const uint8_t idr_only[5] = {0x00, 0x00, 0x00, 0x01, 0x65}; // NAL type 5 without SPS
    const uint8_t idr_no_sps[10] = {
        0x00, 0x00, 0x00, 0x01, 0x65,
        0x00, 0x00, 0x00, 0x01, 0x65,
    }; // Starts with IDR (5) containing IDR slice (5) but missing leading SPS (7)
    const uint8_t sps_pps_idr[16] = {
        0x00, 0x00, 0x00, 0x01, 0x67, 0x42,
        0x00, 0x00, 0x00, 0x01, 0x68,
        0x00, 0x00, 0x00, 0x01, 0x65,
    };
    const uint8_t sps_idr_3b[10] = {0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x00, 0x01, 0x65};
    const uint8_t sps_pps_only[11] = {
        0x00, 0x00, 0x00, 0x01, 0x67, 0x42,
        0x00, 0x00, 0x00, 0x01, 0x68,
    };
    const uint8_t sps_bad_inner_fzb[14] = {
        0x00, 0x00, 0x00, 0x01, 0x67, 0x42,
        0x00, 0x00, 0x01, 0xE8, // corrupted PPS forbidden_zero_bit before IDR
        0x00, 0x00, 0x01, 0x65,
    };
    const uint8_t sps_bad_idr_fzb[10] = {
        0x00, 0x00, 0x00, 0x01, 0x67, 0x42,
        0x00, 0x00, 0x01, 0xE5, // corrupted IDR forbidden_zero_bit
    };
    const uint8_t p_slc[5] = {0x00, 0x00, 0x00, 0x01, 0x41}; // NAL type 1 (non-IDR slice)
    const uint8_t p_slc_noref[5] = {0x00, 0x00, 0x00, 0x01, 0x01};
    const uint8_t pps_only[5] = {0x00, 0x00, 0x00, 0x01, 0x68}; // NAL type 8 (PPS)

    TEST_ASSERT_EQUAL_INT(CSI_H264_FRAME_OK,
                          csi_h264_check_frame(sps_pps_idr, sizeof(sps_pps_idr), true, 1024u));
    TEST_ASSERT_EQUAL_INT(CSI_H264_FRAME_OK,
                          csi_h264_check_frame(sps_idr_3b, sizeof(sps_idr_3b), true, 1024u));
    TEST_ASSERT_EQUAL_INT(CSI_H264_FRAME_ERR_FORMAT, csi_h264_check_frame(sps_only, 5u, true, 1024u));
    TEST_ASSERT_EQUAL_INT(CSI_H264_FRAME_ERR_FORMAT, csi_h264_check_frame(idr_only, 5u, true, 1024u));
    TEST_ASSERT_EQUAL_INT(CSI_H264_FRAME_ERR_FORMAT,
                          csi_h264_check_frame(idr_no_sps, sizeof(idr_no_sps), true, 1024u));
    TEST_ASSERT_EQUAL_INT(CSI_H264_FRAME_ERR_FORMAT,
                          csi_h264_check_frame(sps_pps_only, sizeof(sps_pps_only), true, 1024u));
    TEST_ASSERT_EQUAL_INT(CSI_H264_FRAME_ERR_FORMAT,
                          csi_h264_check_frame(sps_bad_inner_fzb, sizeof(sps_bad_inner_fzb), true, 1024u));
    TEST_ASSERT_EQUAL_INT(CSI_H264_FRAME_ERR_FORMAT,
                          csi_h264_check_frame(sps_bad_idr_fzb, sizeof(sps_bad_idr_fzb), true, 1024u));
    TEST_ASSERT_EQUAL_INT(CSI_H264_FRAME_ERR_FORMAT, csi_h264_check_frame(p_slc, 5u, true, 1024u));
    TEST_ASSERT_EQUAL_INT(CSI_H264_FRAME_ERR_FORMAT, csi_h264_check_frame(pps_only, 5u, true, 1024u));

    TEST_ASSERT_EQUAL_INT(CSI_H264_FRAME_OK, csi_h264_check_frame(p_slc, 5u, false, 1024u));
    TEST_ASSERT_EQUAL_INT(CSI_H264_FRAME_OK, csi_h264_check_frame(p_slc_noref, 5u, false, 1024u));

    // Exact bound vs oversize
    TEST_ASSERT_EQUAL_INT(CSI_H264_FRAME_OK, csi_h264_check_frame(p_slc, 5u, false, 5u));
    TEST_ASSERT_EQUAL_INT(CSI_H264_FRAME_ERR_OVERSIZE, csi_h264_check_frame(p_slc, 5u, false, 4u));
    TEST_ASSERT_EQUAL_INT(CSI_H264_FRAME_ERR_OVERSIZE,
                          csi_h264_check_frame(sps_pps_idr, sizeof(sps_pps_idr), true, 0u));

    // Prefix scan stops after valid IDR and obeys CSI_H264_KEY_SCAN_LIMIT boundaries.
    uint8_t big_au[CSI_H264_KEY_SCAN_LIMIT + 1024u];
    for (size_t i = 0; i < sizeof(big_au); i++) {
        big_au[i] = 0x5Au;
    }
    big_au[0] = 0x00u; big_au[1] = 0x00u; big_au[2] = 0x00u; big_au[3] = 0x01u; big_au[4] = 0x67u;
    big_au[5] = 0x42u;
    big_au[6] = 0x00u; big_au[7] = 0x00u; big_au[8] = 0x01u; big_au[9] = 0x65u;
    // Early exit after valid IDR: bytes after IDR within CSI_H264_KEY_SCAN_LIMIT are not scanned.
    big_au[64] = 0x00u;
    big_au[65] = 0x00u;
    big_au[66] = 0x01u;
    big_au[67] = 0xE5u;
    // Emulation-like invalid pattern deep in payload after valid IDR must not fail check_frame.
    big_au[CSI_H264_KEY_SCAN_LIMIT + 100u] = 0x00u;
    big_au[CSI_H264_KEY_SCAN_LIMIT + 101u] = 0x00u;
    big_au[CSI_H264_KEY_SCAN_LIMIT + 102u] = 0x01u;
    big_au[CSI_H264_KEY_SCAN_LIMIT + 103u] = 0xE5u;
    TEST_ASSERT_EQUAL_INT(CSI_H264_FRAME_OK,
                          csi_h264_check_frame(big_au, sizeof(big_au), true, sizeof(big_au)));

    // Boundary: IDR header byte at CSI_H264_KEY_SCAN_LIMIT - 1 succeeds; at CSI_H264_KEY_SCAN_LIMIT fails.
    uint8_t lim_au[CSI_H264_KEY_SCAN_LIMIT + 64u];
    for (size_t i = 0; i < sizeof(lim_au); i++) {
        lim_au[i] = 0x11u;
    }
    lim_au[0] = 0x00u; lim_au[1] = 0x00u; lim_au[2] = 0x00u; lim_au[3] = 0x01u; lim_au[4] = 0x67u;
    lim_au[CSI_H264_KEY_SCAN_LIMIT - 4u] = 0x00u;
    lim_au[CSI_H264_KEY_SCAN_LIMIT - 3u] = 0x00u;
    lim_au[CSI_H264_KEY_SCAN_LIMIT - 2u] = 0x01u;
    lim_au[CSI_H264_KEY_SCAN_LIMIT - 1u] = 0x65u;
    TEST_ASSERT_EQUAL_INT(CSI_H264_FRAME_OK,
                          csi_h264_check_frame(lim_au, sizeof(lim_au), true, sizeof(lim_au)));

    lim_au[CSI_H264_KEY_SCAN_LIMIT - 4u] = 0x11u;
    lim_au[CSI_H264_KEY_SCAN_LIMIT - 3u] = 0x00u;
    lim_au[CSI_H264_KEY_SCAN_LIMIT - 2u] = 0x00u;
    lim_au[CSI_H264_KEY_SCAN_LIMIT - 1u] = 0x01u;
    lim_au[CSI_H264_KEY_SCAN_LIMIT] = 0x65u;
    TEST_ASSERT_EQUAL_INT(CSI_H264_FRAME_ERR_FORMAT,
                          csi_h264_check_frame(lim_au, sizeof(lim_au), true, sizeof(lim_au)));
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
    // Suppose HW unexpectedly returned P instead of IDR: need_idr remains latched,
    // frames_since_idr stays 0, p_frames does not increment, and consec_failures/self_idr_recoveries increment.
    csi_h264_ctrl_end_frame(&c, CSI_H264_OUTCOME_PUBLISHED, false);
    TEST_ASSERT_EQUAL_UINT(4u, c.self_idr_recoveries);
    TEST_ASSERT_EQUAL_UINT(0u, c.p_frames);
    TEST_ASSERT_EQUAL_UINT(0u, c.frames_since_idr);
    TEST_ASSERT_FALSE(csi_h264_ctrl_begin_frame(&c, 1920u, 1080u, &use_h264, &force_idr));
    TEST_ASSERT_TRUE(force_idr);
    csi_h264_ctrl_end_frame(&c, CSI_H264_OUTCOME_PUBLISHED, true);
    TEST_ASSERT_EQUAL_UINT(0u, c.consec_failures);

    // MJPEG fallback transitions active_codec -> MJPEG with OOM cooldown
    // without incrementing self_idr_recoveries.
    csi_h264_ctrl_end_frame(&c, CSI_H264_OUTCOME_FALLBACK_MJPEG, false);
    TEST_ASSERT_EQUAL_UINT(4u, c.self_idr_recoveries);
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

static void test_ctrl_consec_failures_fallback_and_reset(void)
{
    csi_h264_ctrl_t c;
    bool use_h264 = false;
    bool force_idr = false;
    csi_h264_ctrl_init(&c, true, 10u);
    csi_h264_ctrl_set_codec(&c, CSI_H264_CODEC_H264);

    TEST_ASSERT_TRUE(csi_h264_ctrl_begin_frame(&c, 1280u, 720u, &use_h264, &force_idr));
    TEST_ASSERT_TRUE(use_h264);
    TEST_ASSERT_TRUE(force_idr);
    csi_h264_ctrl_end_frame(&c, CSI_H264_OUTCOME_PUBLISHED, true);

    // 4 (< MAX_CONSEC_FAILURES) failures keep H.264 active, then a good IDR resets count.
    for (uint32_t i = 0u; i < CSI_H264_MAX_CONSEC_FAILURES - 1u; i++) {
        TEST_ASSERT_FALSE(csi_h264_ctrl_begin_frame(&c, 1280u, 720u, &use_h264, &force_idr));
        TEST_ASSERT_TRUE(use_h264);
        csi_h264_ctrl_end_frame(&c, CSI_H264_OUTCOME_ENCODE_ERR, false);
        TEST_ASSERT_EQUAL_INT(CSI_H264_CODEC_H264, csi_h264_ctrl_get_active_codec(&c));
    }
    TEST_ASSERT_EQUAL_UINT(CSI_H264_MAX_CONSEC_FAILURES - 1u, c.consec_failures);
    TEST_ASSERT_FALSE(csi_h264_ctrl_begin_frame(&c, 1280u, 720u, &use_h264, &force_idr));
    TEST_ASSERT_TRUE(force_idr);
    csi_h264_ctrl_end_frame(&c, CSI_H264_OUTCOME_PUBLISHED, true);
    TEST_ASSERT_EQUAL_UINT(0u, c.consec_failures);

    // Mode reset also clears consec_failures (even after 4 failures).
    for (uint32_t i = 0u; i < CSI_H264_MAX_CONSEC_FAILURES - 1u; i++) {
        csi_h264_ctrl_begin_frame(&c, 1280u, 720u, &use_h264, &force_idr);
        csi_h264_ctrl_end_frame(&c, CSI_H264_OUTCOME_OVERSIZE, false);
    }
    TEST_ASSERT_EQUAL_UINT(CSI_H264_MAX_CONSEC_FAILURES - 1u, c.consec_failures);
    csi_h264_ctrl_on_mode_reset(&c);
    TEST_ASSERT_EQUAL_UINT(0u, c.consec_failures);

    // PUBLISH_BUSY does not count toward consec_failures and resets cleanly on IDR.
    TEST_ASSERT_FALSE(csi_h264_ctrl_begin_frame(&c, 1280u, 720u, &use_h264, &force_idr));
    TEST_ASSERT_TRUE(force_idr);
    csi_h264_ctrl_end_frame(&c, CSI_H264_OUTCOME_PUBLISH_BUSY, true);
    TEST_ASSERT_EQUAL_UINT(0u, c.consec_failures);

    // Mixed non-OOM failures (ENCODE_ERR, OVERSIZE, unexpected P on external request / GOP)
    // trigger fallback to MJPEG on the 5th consecutive failure and drain keyframe_req.
    csi_h264_ctrl_begin_frame(&c, 1280u, 720u, &use_h264, &force_idr);
    csi_h264_ctrl_end_frame(&c, CSI_H264_OUTCOME_PUBLISHED, true);

    csi_h264_ctrl_request_keyframe(&c);
    TEST_ASSERT_FALSE(csi_h264_ctrl_begin_frame(&c, 1280u, 720u, &use_h264, &force_idr));
    TEST_ASSERT_TRUE(force_idr);
    // Unexpected P when external request forced IDR counts as failure #1.
    csi_h264_ctrl_end_frame(&c, CSI_H264_OUTCOME_PUBLISHED, false);
    TEST_ASSERT_EQUAL_UINT(1u, c.consec_failures);
    TEST_ASSERT_EQUAL_UINT(0u, c.p_frames);

    csi_h264_ctrl_begin_frame(&c, 1280u, 720u, &use_h264, &force_idr);
    csi_h264_ctrl_end_frame(&c, CSI_H264_OUTCOME_OVERSIZE, false); // #2
    csi_h264_ctrl_begin_frame(&c, 1280u, 720u, &use_h264, &force_idr);
    csi_h264_ctrl_end_frame(&c, CSI_H264_OUTCOME_ENCODE_ERR, false); // #3
    csi_h264_ctrl_begin_frame(&c, 1280u, 720u, &use_h264, &force_idr);
    csi_h264_ctrl_end_frame(&c, CSI_H264_OUTCOME_OVERSIZE, false); // #4
    TEST_ASSERT_EQUAL_INT(CSI_H264_CODEC_H264, csi_h264_ctrl_get_active_codec(&c));

    // 5th consecutive failure with a pending keyframe_req AFTER begin_frame must fall back and clear keyframe_req.
    uint64_t reqs_before = c.keyframe_reqs_applied;
    csi_h264_ctrl_begin_frame(&c, 1280u, 720u, &use_h264, &force_idr);
    csi_h264_ctrl_request_keyframe(&c);
    TEST_ASSERT_EQUAL_UINT(1u, c.keyframe_req);
    csi_h264_ctrl_end_frame(&c, CSI_H264_OUTCOME_ENCODE_ERR, false); // #5 -> fallback
    TEST_ASSERT_EQUAL_UINT(0u, c.consec_failures);
    TEST_ASSERT_EQUAL_UINT(0u, c.keyframe_req);
    TEST_ASSERT_EQUAL_INT(CSI_H264_CODEC_MJPEG, csi_h264_ctrl_get_active_codec(&c));
    TEST_ASSERT_EQUAL_UINT(1u, c.switches_to_mjpeg);
    TEST_ASSERT_EQUAL_UINT(CSI_H264_OOM_RETRY_FRAMES, c.oom_cooldown_frames);

    // Stay in MJPEG for CSI_H264_OOM_RETRY_FRAMES frames, then retry H.264 with IDR forced.
    for (uint32_t i = 0u; i < CSI_H264_OOM_RETRY_FRAMES; i++) {
        TEST_ASSERT_FALSE(csi_h264_ctrl_begin_frame(&c, 1280u, 720u, &use_h264, &force_idr));
        TEST_ASSERT_FALSE(use_h264);
    }
    TEST_ASSERT_TRUE(csi_h264_ctrl_begin_frame(&c, 1280u, 720u, &use_h264, &force_idr));
    TEST_ASSERT_TRUE(use_h264);
    TEST_ASSERT_TRUE(force_idr);
    TEST_ASSERT_EQUAL_UINT64(reqs_before, c.keyframe_reqs_applied);

    // Verify immediate mode reset after a 5th-failure fallback also does not apply a stale keyframe_req.
    for (uint32_t i = 0u; i < CSI_H264_MAX_CONSEC_FAILURES; i++) {
        csi_h264_ctrl_begin_frame(&c, 1280u, 720u, &use_h264, &force_idr);
        if (i + 1u == CSI_H264_MAX_CONSEC_FAILURES) {
            csi_h264_ctrl_request_keyframe(&c);
            TEST_ASSERT_EQUAL_UINT(1u, c.keyframe_req);
        }
        csi_h264_ctrl_end_frame(&c, CSI_H264_OUTCOME_ENCODE_ERR, false);
    }
    TEST_ASSERT_EQUAL_UINT(0u, c.keyframe_req);
    csi_h264_ctrl_on_mode_reset(&c);
    TEST_ASSERT_TRUE(csi_h264_ctrl_begin_frame(&c, 1280u, 720u, &use_h264, &force_idr));
    TEST_ASSERT_TRUE(use_h264);
    TEST_ASSERT_EQUAL_UINT64(reqs_before, c.keyframe_reqs_applied);
}

static void test_ctrl_runtime_disable_h264(void)
{
    csi_h264_ctrl_t c;
    bool use_h264 = false;
    bool force_idr = false;

    csi_h264_ctrl_disable_h264(NULL);

    csi_h264_ctrl_init(&c, true, 30u);
    TEST_ASSERT_EQUAL_INT(0, csi_h264_ctrl_set_codec(&c, CSI_H264_CODEC_H264));
    TEST_ASSERT_TRUE(csi_h264_ctrl_begin_frame(&c, 1280u, 720u, &use_h264, &force_idr));
    TEST_ASSERT_TRUE(use_h264);
    csi_h264_ctrl_end_frame(&c, CSI_H264_OUTCOME_PUBLISHED, true);
    csi_h264_ctrl_request_keyframe(&c);
    TEST_ASSERT_EQUAL_UINT(1u, c.keyframe_req);

    // Runtime disable (e.g. csi_h264_create() failed): switches to MJPEG and prevents re-enabling.
    csi_h264_ctrl_disable_h264(&c);
    TEST_ASSERT_FALSE(c.h264_capable);
    TEST_ASSERT_EQUAL_INT(CSI_H264_CODEC_MJPEG, csi_h264_ctrl_get_active_codec(&c));
    TEST_ASSERT_EQUAL_UINT(0u, c.keyframe_req);
    TEST_ASSERT_EQUAL_INT(-1, csi_h264_ctrl_set_codec(&c, CSI_H264_CODEC_H264));
    TEST_ASSERT_EQUAL_INT(0, csi_h264_ctrl_set_codec(&c, CSI_H264_CODEC_MJPEG));
    csi_h264_ctrl_request_keyframe(&c);
    TEST_ASSERT_EQUAL_UINT(0u, c.keyframe_req);

    use_h264 = true;
    force_idr = true;
    TEST_ASSERT_FALSE(csi_h264_ctrl_begin_frame(&c, 1280u, 720u, &use_h264, &force_idr));
    TEST_ASSERT_FALSE(use_h264);
    TEST_ASSERT_FALSE(force_idr);
}

typedef struct {
    const uint8_t *buf;
    size_t len;
    size_t bit_pos;
} test_br_t;

static uint32_t test_br_u(test_br_t *br, unsigned n)
{
    uint32_t v = 0u;
    for (unsigned i = 0u; i < n; i++) {
        TEST_ASSERT_LESS_THAN_UINT(br->len * 8u, br->bit_pos);
        uint32_t b = (br->buf[br->bit_pos >> 3] >> (7u - (br->bit_pos & 7u))) & 1u;
        v = (v << 1) | b;
        br->bit_pos++;
    }
    return v;
}

static uint32_t test_br_ue(test_br_t *br)
{
    unsigned z = 0u;
    while (test_br_u(br, 1u) == 0u) {
        z++;
        TEST_ASSERT_LESS_OR_EQUAL_UINT(16u, z);
    }
    if (z == 0u) {
        return 0u;
    }
    return ((1u << z) - 1u) + test_br_u(br, z);
}

static size_t test_ebsp_to_rbsp(const uint8_t *ebsp, size_t len, uint8_t *rbsp, size_t cap)
{
    size_t out = 0u;
    int zeros = 0;
    for (size_t i = 0u; i < len; i++) {
        uint8_t b = ebsp[i];
        if (zeros >= 2) {
            TEST_ASSERT_TRUE_MESSAGE(b >= 0x03u, "Illegal 00 00 00/01/02 start-code emulation in EBSP");
            if (b == 0x03u) {
                zeros = 0;
                continue;
            }
        }
        TEST_ASSERT_LESS_THAN_UINT(cap, out);
        rbsp[out++] = b;
        zeros = (b == 0u) ? (zeros + 1) : 0;
    }
    return out;
}

static void assert_patched_sps_fields(const uint8_t *ebsp, size_t ebsp_len,
                                      uint8_t exp_level, uint32_t exp_w_mbs_minus1,
                                      uint32_t exp_h_mbs_minus1, uint32_t exp_crop,
                                      uint32_t exp_crop_bottom, uint32_t exp_time_scale)
{
    uint8_t rbsp[64];
    size_t rbsp_len = test_ebsp_to_rbsp(ebsp, ebsp_len, rbsp, sizeof(rbsp));
    test_br_t br = { .buf = rbsp, .len = rbsp_len, .bit_pos = 0u };

    TEST_ASSERT_EQUAL_UINT(0x67u, test_br_u(&br, 8u));
    TEST_ASSERT_EQUAL_UINT(66u, test_br_u(&br, 8u));
    TEST_ASSERT_EQUAL_UINT(0xc0u, test_br_u(&br, 8u));
    TEST_ASSERT_EQUAL_UINT(exp_level, test_br_u(&br, 8u));
    TEST_ASSERT_EQUAL_UINT(0u, test_br_ue(&br)); /* sps_id */
    TEST_ASSERT_EQUAL_UINT(4u, test_br_ue(&br)); /* log2_max_frame_num_minus4 */
    TEST_ASSERT_EQUAL_UINT(2u, test_br_ue(&br)); /* pic_order_cnt_type */
    TEST_ASSERT_EQUAL_UINT(1u, test_br_ue(&br)); /* max_num_ref_frames */
    TEST_ASSERT_EQUAL_UINT(0u, test_br_u(&br, 1u));
    TEST_ASSERT_EQUAL_UINT(exp_w_mbs_minus1, test_br_ue(&br));
    TEST_ASSERT_EQUAL_UINT(exp_h_mbs_minus1, test_br_ue(&br));
    TEST_ASSERT_EQUAL_UINT(1u, test_br_u(&br, 1u)); /* frame_mbs_only */
    TEST_ASSERT_EQUAL_UINT(0u, test_br_u(&br, 1u)); /* direct_8x8 */
    uint32_t crop = test_br_u(&br, 1u);
    TEST_ASSERT_EQUAL_UINT(exp_crop, crop);
    if (crop != 0u) {
        TEST_ASSERT_EQUAL_UINT(0u, test_br_ue(&br));
        TEST_ASSERT_EQUAL_UINT(0u, test_br_ue(&br));
        TEST_ASSERT_EQUAL_UINT(0u, test_br_ue(&br));
        TEST_ASSERT_EQUAL_UINT(exp_crop_bottom, test_br_ue(&br));
    }
    TEST_ASSERT_EQUAL_UINT(1u, test_br_u(&br, 1u)); /* vui_parameters_present_flag */
    TEST_ASSERT_EQUAL_UINT(0u, test_br_u(&br, 1u)); /* aspect_ratio */
    TEST_ASSERT_EQUAL_UINT(0u, test_br_u(&br, 1u)); /* overscan */
    TEST_ASSERT_EQUAL_UINT(0u, test_br_u(&br, 1u)); /* video_signal_type */
    TEST_ASSERT_EQUAL_UINT(0u, test_br_u(&br, 1u)); /* chroma_loc */
    uint32_t ti = test_br_u(&br, 1u);
    if (exp_time_scale > 0u) {
        TEST_ASSERT_EQUAL_UINT(1u, ti);
        TEST_ASSERT_EQUAL_UINT(1u, test_br_u(&br, 32u));
        TEST_ASSERT_EQUAL_UINT(exp_time_scale, test_br_u(&br, 32u));
        TEST_ASSERT_EQUAL_UINT(0u, test_br_u(&br, 1u)); /* fixed_frame_rate_flag forced to 0 */
    } else {
        TEST_ASSERT_EQUAL_UINT(0u, ti);
    }
    TEST_ASSERT_EQUAL_UINT(0u, test_br_u(&br, 1u)); /* nal_hrd */
    TEST_ASSERT_EQUAL_UINT(0u, test_br_u(&br, 1u)); /* vcl_hrd */
    TEST_ASSERT_EQUAL_UINT(0u, test_br_u(&br, 1u)); /* pic_struct */
    TEST_ASSERT_EQUAL_UINT(1u, test_br_u(&br, 1u)); /* bitstream_restriction_flag */
    TEST_ASSERT_EQUAL_UINT(1u, test_br_u(&br, 1u)); /* motion_vectors_over_pic_boundaries */
    TEST_ASSERT_EQUAL_UINT(0u, test_br_ue(&br));    /* max_bytes_per_pic_denom */
    TEST_ASSERT_EQUAL_UINT(0u, test_br_ue(&br));    /* max_bits_per_mb_denom */
    TEST_ASSERT_EQUAL_UINT(11u, test_br_ue(&br));   /* log2_max_mv_length_h */
    TEST_ASSERT_EQUAL_UINT(11u, test_br_ue(&br));   /* log2_max_mv_length_v */
    TEST_ASSERT_EQUAL_UINT(0u, test_br_ue(&br));    /* max_num_reorder_frames = 0 */
    TEST_ASSERT_EQUAL_UINT(1u, test_br_ue(&br));    /* max_dec_frame_buffering = 1 */
    TEST_ASSERT_EQUAL_UINT(1u, test_br_u(&br, 1u)); /* rbsp_stop_one_bit */
    while ((br.bit_pos & 7u) != 0u) {
        TEST_ASSERT_EQUAL_UINT(0u, test_br_u(&br, 1u));
    }
    TEST_ASSERT_EQUAL_UINT(rbsp_len * 8u, br.bit_pos);
}

static void test_patch_sps_low_latency(void)
{
    const uint8_t sps_720p20[] = {
        0x67, 0x42, 0xc0, 0x1f, 0x95, 0xa0, 0x14, 0x01, 0x6c, 0x84,
        0x00, 0x00, 0x03, 0x00, 0x04, 0x00, 0x00, 0x03, 0x00, 0xa2, 0x10
    };
    const uint8_t sps_900p14[] = {
        0x67, 0x42, 0xc0, 0x20, 0x95, 0xa0, 0x16, 0x81, 0xcd, 0xe7, 0x84,
        0x00, 0x00, 0x03, 0x00, 0x04, 0x00, 0x00, 0x03, 0x00, 0x72, 0x10
    };
    const uint8_t sps_1080p9[] = {
        0x67, 0x42, 0xc0, 0x29, 0x95, 0xa0, 0x1e, 0x00, 0x89, 0x79, 0x61,
        0x00, 0x00, 0x03, 0x00, 0x01, 0x00, 0x00, 0x03, 0x00, 0x12, 0x84
    };

    uint8_t out[80];
    size_t len = csi_h264_patch_sps_low_latency(sps_720p20, sizeof(sps_720p20), out, sizeof(out));
    TEST_ASSERT_GREATER_THAN_UINT(sizeof(sps_720p20), len);
    assert_patched_sps_fields(out, len, 31u, 79u, 44u, 0u, 0u, 40u);
    /* Re-patching an already-patched SPS is rejected cleanly because bitstream_restriction_flag is 1 */
    uint8_t out2[80];
    TEST_ASSERT_EQUAL_UINT(0u, csi_h264_patch_sps_low_latency(out, len, out2, sizeof(out2)));

    len = csi_h264_patch_sps_low_latency(sps_900p14, sizeof(sps_900p14), out, sizeof(out));
    TEST_ASSERT_GREATER_THAN_UINT(sizeof(sps_900p14), len);
    assert_patched_sps_fields(out, len, 32u, 89u, 56u, 1u, 6u, 28u);

    len = csi_h264_patch_sps_low_latency(sps_1080p9, sizeof(sps_1080p9), out, sizeof(out));
    TEST_ASSERT_GREATER_THAN_UINT(sizeof(sps_1080p9), len);
    assert_patched_sps_fields(out, len, 41u, 119u, 67u, 1u, 4u, 18u);

    /* max_num_ref_frames = 2 (> 1) would violate max_dec_frame_buffering >= max_num_ref_frames: refuse patch */
    uint8_t sps_ref2[sizeof(sps_720p20)];
    memcpy(sps_ref2, sps_720p20, sizeof(sps_720p20));
    sps_ref2[5] = 0xb0u;
    TEST_ASSERT_EQUAL_UINT(0u, csi_h264_patch_sps_low_latency(sps_ref2, sizeof(sps_ref2), out, sizeof(out)));

    /* Non-SPS, non-Baseline, malformed / small buffer rejection */
    const uint8_t pps[] = { 0x68, 0xce, 0x3c, 0x80, 0x00 };
    const uint8_t main_prof[] = { 0x67, 0x4d, 0x40, 0x1f, 0x95, 0xa0 };
    TEST_ASSERT_EQUAL_UINT(0u, csi_h264_patch_sps_low_latency(NULL, 10u, out, sizeof(out)));
    TEST_ASSERT_EQUAL_UINT(0u, csi_h264_patch_sps_low_latency(sps_720p20, sizeof(sps_720p20), NULL, sizeof(out)));
    TEST_ASSERT_EQUAL_UINT(0u, csi_h264_patch_sps_low_latency(sps_720p20, 4u, out, sizeof(out)));
    TEST_ASSERT_EQUAL_UINT(0u, csi_h264_patch_sps_low_latency(pps, sizeof(pps), out, sizeof(out)));
    TEST_ASSERT_EQUAL_UINT(0u, csi_h264_patch_sps_low_latency(main_prof, sizeof(main_prof), out, sizeof(out)));
    TEST_ASSERT_EQUAL_UINT(0u, csi_h264_patch_sps_low_latency(sps_720p20, sizeof(sps_720p20), out, 10u));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_align_up_bounds_and_overflow);
    RUN_TEST(test_check_resolution_boundaries);
    RUN_TEST(test_yuv_and_output_buffer_sizing);
    RUN_TEST(test_target_fps_and_param_validation);
    RUN_TEST(test_patch_sps_low_latency);
    RUN_TEST(test_check_frame_empty_and_start_code);
    RUN_TEST(test_check_frame_key_and_oversize);
    RUN_TEST(test_ctrl_null_and_incapable);
    RUN_TEST(test_ctrl_mjpeg_request_keyframe_is_noop);
    RUN_TEST(test_ctrl_switch_gop_and_request_keyframe);
    RUN_TEST(test_ctrl_self_idr_recovery_and_fallback);
    RUN_TEST(test_ctrl_consec_failures_fallback_and_reset);
    RUN_TEST(test_ctrl_downgrade_clears_pending_request);
    RUN_TEST(test_ctrl_oom_fallback_and_retry);
    RUN_TEST(test_ctrl_runtime_disable_h264);
    RUN_TEST(test_ctrl_fuzz_invariants);
    return UNITY_END();
}
