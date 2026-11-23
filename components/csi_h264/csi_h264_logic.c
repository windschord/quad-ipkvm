#include "csi_h264_logic.h"

size_t csi_h264_align_up(size_t val, size_t align)
{
    if (align == 0u) {
        return 0u;
    }
    size_t rem = val % align;
    if (rem == 0u) {
        return val;
    }
    size_t add = align - rem;
    if (val > SIZE_MAX - add) {
        return 0u;
    }
    return val + add;
}

bool csi_h264_check_resolution(uint32_t w, uint32_t h)
{
    if (w < CSI_H264_MIN_WIDTH || w > CSI_H264_MAX_WIDTH ||
        h < CSI_H264_MIN_HEIGHT || h > CSI_H264_MAX_HEIGHT) {
        return false;
    }
    if ((w & (CSI_H264_ALIGN_DIM - 1u)) != 0u || (h & 1u) != 0u) {
        return false;
    }
    return true;
}

size_t csi_h264_calc_yuv_bytes(uint32_t w, uint32_t h)
{
    if (!csi_h264_check_resolution(w, h)) {
        return 0u;
    }
    size_t aw = csi_h264_align_up((size_t)w, CSI_H264_ALIGN_DIM);
    size_t ah = csi_h264_align_up((size_t)h, CSI_H264_ALIGN_DIM);
    if (aw == 0u || ah == 0u) {
        return 0u;
    }
    return (aw * ah * 3u) / 2u;
}

size_t csi_h264_calc_ppa_active_bytes(uint32_t w, uint32_t h)
{
    if (!csi_h264_check_resolution(w, h)) {
        return 0u;
    }
    size_t aw = csi_h264_align_up((size_t)w, CSI_H264_ALIGN_DIM);
    if (aw == 0u) {
        return 0u;
    }
    return (aw * (size_t)h * 3u) / 2u;
}

size_t csi_h264_calc_output_buf_size(size_t hub_max_frame_bytes)
{
    if (hub_max_frame_bytes == 0u || hub_max_frame_bytes > CSI_H264_MAX_OUT_BYTES) {
        return 0u;
    }
    size_t aligned = csi_h264_align_up(hub_max_frame_bytes, CSI_H264_DMA_ALIGN);
    if (aligned == 0u || aligned > CSI_H264_MAX_OUT_BYTES) {
        return 0u;
    }
    return aligned;
}

uint8_t csi_h264_calc_target_fps(uint32_t w, uint32_t h)
{
    if (!csi_h264_check_resolution(w, h)) {
        return 0u;
    }
    // Measured ESP32-P4 rev v1.3 PPA+H.264 cadence (docs/P7_H264_EVAL.md §3):
    // 720p ~= 20.5 fps, 1080p ~= 9.1 fps. Scale intermediate modes smoothly by pixel area.
    uint64_t pixels = (uint64_t)w * (uint64_t)h;
    uint64_t ref_pixels = (uint64_t)1280u * 720u;
    if (pixels <= ref_pixels) {
        return 20u;
    }
    uint64_t scaled = ((uint64_t)20u * ref_pixels + (pixels / 2u)) / pixels;
    if (scaled < 9u) {
        return 9u;
    }
    if (scaled > 20u) {
        return 20u;
    }
    return (uint8_t)scaled;
}

typedef struct {
    const uint8_t *buf;
    size_t len;
    size_t bit_pos;
    bool err;
} sps_br_t;

typedef struct {
    uint8_t *out;
    size_t cap;
    size_t pos;
    uint8_t cur_byte;
    unsigned bits_filled;
    int zeros;
    bool err;
} sps_bw_t;

static uint32_t sps_br_u(sps_br_t *br, unsigned n)
{
    uint32_t v = 0u;
    for (unsigned i = 0u; i < n; i++) {
        size_t byte_i = br->bit_pos >> 3;
        if (byte_i >= br->len) {
            br->err = true;
            return 0u;
        }
        uint32_t bit = (uint32_t)((br->buf[byte_i] >> (7u - (br->bit_pos & 7u))) & 1u);
        v = (v << 1) | bit;
        br->bit_pos++;
    }
    return v;
}

static uint32_t sps_br_ue(sps_br_t *br)
{
    unsigned zeros = 0u;
    while (!br->err && sps_br_u(br, 1u) == 0u) {
        zeros++;
        if (zeros > 16u) {
            br->err = true;
            return 0u;
        }
    }
    if (br->err || zeros == 0u) {
        return 0u;
    }
    uint32_t suffix = sps_br_u(br, zeros);
    return ((1u << zeros) - 1u) + suffix;
}

static void sps_bw_emit_byte(sps_bw_t *bw, uint8_t b)
{
    if (bw->err) {
        return;
    }
    if (bw->zeros >= 2 && b <= 3u) {
        if (bw->pos >= bw->cap) {
            bw->err = true;
            return;
        }
        bw->out[bw->pos++] = 0x03u;
        bw->zeros = 0;
    }
    if (bw->pos >= bw->cap) {
        bw->err = true;
        return;
    }
    bw->out[bw->pos++] = b;
    bw->zeros = (b == 0u) ? (bw->zeros + 1) : 0;
}

static void sps_bw_u1(sps_bw_t *bw, uint32_t bit)
{
    bw->cur_byte = (uint8_t)((bw->cur_byte << 1) | (bit & 1u));
    bw->bits_filled++;
    if (bw->bits_filled == 8u) {
        sps_bw_emit_byte(bw, bw->cur_byte);
        bw->cur_byte = 0u;
        bw->bits_filled = 0u;
    }
}

static void sps_bw_u(sps_bw_t *bw, uint32_t val, unsigned n)
{
    for (unsigned i = n; i > 0u; i--) {
        sps_bw_u1(bw, (val >> (i - 1u)) & 1u);
    }
}

static void sps_bw_ue(sps_bw_t *bw, uint32_t val)
{
    if (val == 0u) {
        sps_bw_u1(bw, 1u);
        return;
    }
    uint32_t code = val + 1u;
    unsigned bits = 0u;
    for (uint32_t t = code; t > 0u; t >>= 1) {
        bits++;
    }
    sps_bw_u(bw, 0u, bits - 1u);
    sps_bw_u(bw, code, bits);
}

static void sps_copy_u(sps_br_t *br, sps_bw_t *bw, unsigned n)
{
    sps_bw_u(bw, sps_br_u(br, n), n);
}

static void sps_copy_ue(sps_br_t *br, sps_bw_t *bw)
{
    sps_bw_ue(bw, sps_br_ue(br));
}

size_t csi_h264_patch_sps_low_latency(const uint8_t *sps_in, size_t sps_in_len,
                                      uint8_t *sps_out, size_t sps_out_cap)
{
    if (!sps_in || !sps_out || sps_in_len < 5u || sps_in_len > 64u || sps_out_cap < 8u) {
        return 0u;
    }

    uint8_t rbsp[64];
    size_t rbsp_len = 0u;
    int zeros = 0;
    for (size_t i = 0u; i < sps_in_len; i++) {
        uint8_t b = sps_in[i];
        if (zeros >= 2) {
            if (b == 0x03u) {
                zeros = 0;
                continue;
            }
            if (b < 0x03u) {
                return 0u;
            }
        }
        rbsp[rbsp_len++] = b;
        zeros = (b == 0u) ? (zeros + 1) : 0;
    }
    if (rbsp_len < 4u || (rbsp[0] & 0x80u) != 0u || (rbsp[0] & 0x1Fu) != 7u || rbsp[1] != 66u) {
        return 0u;
    }

    sps_br_t br = { .buf = rbsp, .len = rbsp_len, .bit_pos = 0u, .err = false };
    sps_bw_t bw = { .out = sps_out, .cap = sps_out_cap, .pos = 0u, .cur_byte = 0u, .bits_filled = 0u, .zeros = 0, .err = false };

    sps_copy_u(&br, &bw, 32u); /* nal_hdr, profile_idc, constraints, level_idc */
    sps_copy_ue(&br, &bw);     /* seq_parameter_set_id */
    sps_copy_ue(&br, &bw);     /* log2_max_frame_num_minus4 */
    uint32_t poc_type = sps_br_ue(&br);
    sps_bw_ue(&bw, poc_type);
    if (poc_type == 0u) {
        sps_copy_ue(&br, &bw); /* log2_max_pic_order_cnt_lsb_minus4 */
    } else if (poc_type != 2u) {
        return 0u;
    }
    sps_copy_ue(&br, &bw);     /* max_num_ref_frames */
    sps_copy_u(&br, &bw, 1u);  /* gaps_in_frame_num_value_allowed_flag */
    sps_copy_ue(&br, &bw);     /* pic_width_in_mbs_minus1 */
    sps_copy_ue(&br, &bw);     /* pic_height_in_map_units_minus1 */
    uint32_t frame_mbs_only = sps_br_u(&br, 1u);
    sps_bw_u1(&bw, frame_mbs_only);
    if (frame_mbs_only == 0u) {
        sps_copy_u(&br, &bw, 1u); /* mb_adaptive_frame_field_flag */
    }
    sps_copy_u(&br, &bw, 1u);  /* direct_8x8_inference_flag */
    uint32_t crop = sps_br_u(&br, 1u);
    sps_bw_u1(&bw, crop);
    if (crop != 0u) {
        for (int c = 0; c < 4; c++) {
            sps_copy_ue(&br, &bw);
        }
    }

    uint32_t vui = sps_br_u(&br, 1u);
    sps_bw_u1(&bw, 1u); /* vui_parameters_present_flag = 1 */
    if (vui != 0u) {
        uint32_t ar = sps_br_u(&br, 1u);
        sps_bw_u1(&bw, ar);
        if (ar != 0u) {
            uint32_t idc = sps_br_u(&br, 8u);
            sps_bw_u(&bw, idc, 8u);
            if (idc == 255u) {
                sps_copy_u(&br, &bw, 32u); /* sar_width + sar_height */
            }
        }
        uint32_t overscan = sps_br_u(&br, 1u);
        sps_bw_u1(&bw, overscan);
        if (overscan != 0u) {
            sps_copy_u(&br, &bw, 1u);
        }
        uint32_t vst = sps_br_u(&br, 1u);
        sps_bw_u1(&bw, vst);
        if (vst != 0u) {
            sps_copy_u(&br, &bw, 4u); /* video_format(3) + video_full_range_flag(1) */
            uint32_t cdesc = sps_br_u(&br, 1u);
            sps_bw_u1(&bw, cdesc);
            if (cdesc != 0u) {
                sps_copy_u(&br, &bw, 24u);
            }
        }
        uint32_t chroma = sps_br_u(&br, 1u);
        sps_bw_u1(&bw, chroma);
        if (chroma != 0u) {
            sps_copy_ue(&br, &bw);
            sps_copy_ue(&br, &bw);
        }
        uint32_t ti = sps_br_u(&br, 1u);
        sps_bw_u1(&bw, ti);
        if (ti != 0u) {
            sps_copy_u(&br, &bw, 32u); /* num_units_in_tick */
            sps_copy_u(&br, &bw, 32u); /* time_scale */
            (void)sps_br_u(&br, 1u);   /* original fixed_frame_rate_flag */
            sps_bw_u1(&bw, 0u);        /* variable cadence -> no decoder VSync pacing buffer */
        }
        uint32_t nal_hrd = sps_br_u(&br, 1u);
        uint32_t vcl_hrd = sps_br_u(&br, 1u);
        if (nal_hrd != 0u || vcl_hrd != 0u) {
            return 0u;
        }
        sps_bw_u1(&bw, 0u);
        sps_bw_u1(&bw, 0u);
        sps_copy_u(&br, &bw, 1u); /* pic_struct_present_flag */
        if (sps_br_u(&br, 1u) != 0u) {
            return 0u; /* already has bitstream_restriction_flag */
        }
    } else {
        sps_bw_u(&bw, 0u, 8u); /* ar, overscan, vst, chroma, ti, nal_hrd, vcl_hrd, pic_struct */
    }

    /* bitstream_restriction_flag = 1 with zero frame reordering and 1-frame DPB */
    sps_bw_u1(&bw, 1u);
    sps_bw_u1(&bw, 1u);   /* motion_vectors_over_pic_boundaries_flag */
    sps_bw_ue(&bw, 0u);   /* max_bytes_per_pic_denom */
    sps_bw_ue(&bw, 0u);   /* max_bits_per_mb_denom */
    sps_bw_ue(&bw, 11u);  /* log2_max_mv_length_horizontal */
    sps_bw_ue(&bw, 11u);  /* log2_max_mv_length_vertical */
    sps_bw_ue(&bw, 0u);   /* max_num_reorder_frames */
    sps_bw_ue(&bw, 1u);   /* max_dec_frame_buffering */

    /* Verify exact rbsp_trailing_bits alignment in input */
    if (sps_br_u(&br, 1u) != 1u) {
        return 0u;
    }
    while ((br.bit_pos & 7u) != 0u) {
        if (sps_br_u(&br, 1u) != 0u) {
            return 0u;
        }
    }
    if (br.err || (br.bit_pos >> 3) != rbsp_len) {
        return 0u;
    }

    /* Emit rbsp_trailing_bits into output */
    sps_bw_u1(&bw, 1u);
    while (bw.bits_filled != 0u) {
        sps_bw_u1(&bw, 0u);
    }
    return bw.err ? 0u : bw.pos;
}

bool csi_h264_validate_enc_params(uint32_t w, uint32_t h, uint8_t gop, uint8_t fps,
                                  uint32_t bitrate_bps, uint8_t qp_min, uint8_t qp_max)
{
    if (!csi_h264_check_resolution(w, h)) {
        return false;
    }
    if (gop < CSI_H264_MIN_GOP || fps == 0u || bitrate_bps == 0u) {
        return false;
    }
    if (qp_min > qp_max || qp_max > CSI_H264_MAX_QP) {
        return false;
    }
    return true;
}

csi_h264_frame_err_t csi_h264_check_frame(const uint8_t *data, size_t len,
                                          bool expect_key, size_t max_frame_bytes)
{
    if (!data || len < 5u) {
        return CSI_H264_FRAME_ERR_EMPTY;
    }
    // Annex B 4-byte start code 00 00 00 01 (docs/VIDEO_TRANSPORT.md §9.3).
    if (data[0] != 0x00u || data[1] != 0x00u || data[2] != 0x00u || data[3] != 0x01u) {
        return CSI_H264_FRAME_ERR_FORMAT;
    }
    uint8_t nal_hdr = data[4];
    // H.264 NAL forbidden_zero_bit must be 0.
    if ((nal_hdr & 0x80u) != 0u) {
        return CSI_H264_FRAME_ERR_FORMAT;
    }
    if (expect_key) {
        uint8_t nal_type = nal_hdr & 0x1Fu;
        // HW IDR access unit starts with SPS (7) and contains at least one IDR slice (5).
        if (nal_type != 7u) {
            return CSI_H264_FRAME_ERR_FORMAT;
        }
        bool found_idr = false;
        size_t scan_end = (len < CSI_H264_KEY_SCAN_LIMIT) ? len : CSI_H264_KEY_SCAN_LIMIT;
        size_t i = 5u;
        while (i + 3u < scan_end) {
            if (data[i] == 0x00u && data[i + 1u] == 0x00u) {
                size_t hdr_idx = 0u;
                if (data[i + 2u] == 0x01u && (i + 3u < scan_end)) {
                    hdr_idx = i + 3u;
                } else if (data[i + 2u] == 0x00u && (i + 4u < scan_end) && data[i + 3u] == 0x01u) {
                    hdr_idx = i + 4u;
                }
                if (hdr_idx != 0u) {
                    uint8_t nh = data[hdr_idx];
                    if ((nh & 0x80u) != 0u) {
                        return CSI_H264_FRAME_ERR_FORMAT;
                    }
                    if ((nh & 0x1Fu) == 5u) {
                        found_idr = true;
                        break;
                    }
                    i = hdr_idx + 1u;
                    continue;
                }
            }
            i++;
        }
        if (!found_idr) {
            return CSI_H264_FRAME_ERR_FORMAT;
        }
    }
    if (max_frame_bytes == 0u || len > max_frame_bytes) {
        return CSI_H264_FRAME_ERR_OVERSIZE;
    }
    return CSI_H264_FRAME_OK;
}

void csi_h264_ctrl_init(csi_h264_ctrl_t *c, bool h264_capable, uint8_t gop)
{
    if (!c) {
        return;
    }
    __atomic_store_n(&c->h264_capable, h264_capable, __ATOMIC_SEQ_CST);
    c->gop = (gop < CSI_H264_MIN_GOP) ? CSI_H264_MIN_GOP : gop;
    __atomic_store_n(&c->requested_codec, (uint32_t)CSI_H264_CODEC_MJPEG, __ATOMIC_SEQ_CST);
    __atomic_store_n(&c->requested_epoch, 0u, __ATOMIC_SEQ_CST);
    __atomic_store_n(&c->active_codec, (uint32_t)CSI_H264_CODEC_MJPEG, __ATOMIC_SEQ_CST);
    __atomic_store_n(&c->keyframe_req, 0u, __ATOMIC_SEQ_CST);

    c->need_idr = false;
    c->active_w = 0u;
    c->active_h = 0u;
    c->frames_since_idr = 0u;
    c->oom_cooldown_frames = 0u;
    c->consec_failures = 0u;
    c->seen_req_epoch = 0u;

    c->switches_to_h264 = 0u;
    c->switches_to_mjpeg = 0u;
    c->idr_frames = 0u;
    c->p_frames = 0u;
    c->keyframe_reqs_applied = 0u;
    c->self_idr_recoveries = 0u;
}

void csi_h264_ctrl_disable_h264(csi_h264_ctrl_t *c)
{
    if (!c) {
        return;
    }
    __atomic_store_n(&c->h264_capable, false, __ATOMIC_RELEASE);
    __atomic_store_n(&c->active_codec, (uint32_t)CSI_H264_CODEC_MJPEG, __ATOMIC_RELEASE);
    (void)__atomic_exchange_n(&c->keyframe_req, 0u, __ATOMIC_ACQ_REL);
    c->need_idr = false;
    c->frames_since_idr = 0u;
    c->oom_cooldown_frames = 0u;
    c->consec_failures = 0u;
}

int csi_h264_ctrl_set_codec(csi_h264_ctrl_t *c, csi_h264_codec_t codec)
{
    if (!c) {
        return -1;
    }
    if (codec == CSI_H264_CODEC_MJPEG) {
        __atomic_store_n(&c->requested_codec, (uint32_t)CSI_H264_CODEC_MJPEG, __ATOMIC_RELEASE);
        __atomic_fetch_add(&c->requested_epoch, 1u, __ATOMIC_RELEASE);
        return 0;
    }
    if (codec == CSI_H264_CODEC_H264 && __atomic_load_n(&c->h264_capable, __ATOMIC_ACQUIRE)) {
        __atomic_store_n(&c->requested_codec, (uint32_t)CSI_H264_CODEC_H264, __ATOMIC_RELEASE);
        __atomic_fetch_add(&c->requested_epoch, 1u, __ATOMIC_RELEASE);
        return 0;
    }
    return -1;
}

void csi_h264_ctrl_request_keyframe(csi_h264_ctrl_t *c)
{
    if (!c || !__atomic_load_n(&c->h264_capable, __ATOMIC_ACQUIRE)) {
        return;
    }
    // Wait-free / lock-free check: in MJPEG mode request_keyframe is strictly a no-op
    // (docs/VIDEO_TRANSPORT.md §9.6-9.7).
    uint32_t active = __atomic_load_n(&c->active_codec, __ATOMIC_ACQUIRE);
    if (active != (uint32_t)CSI_H264_CODEC_H264) {
        return;
    }
    __atomic_store_n(&c->keyframe_req, 1u, __ATOMIC_RELEASE);
}

csi_h264_codec_t csi_h264_ctrl_get_requested_codec(const csi_h264_ctrl_t *c)
{
    if (!c) {
        return CSI_H264_CODEC_MJPEG;
    }
    uint32_t val = __atomic_load_n(&c->requested_codec, __ATOMIC_ACQUIRE);
    return (val == (uint32_t)CSI_H264_CODEC_H264) ? CSI_H264_CODEC_H264 : CSI_H264_CODEC_MJPEG;
}

csi_h264_codec_t csi_h264_ctrl_get_active_codec(const csi_h264_ctrl_t *c)
{
    if (!c) {
        return CSI_H264_CODEC_MJPEG;
    }
    uint32_t val = __atomic_load_n(&c->active_codec, __ATOMIC_ACQUIRE);
    return (val == (uint32_t)CSI_H264_CODEC_H264) ? CSI_H264_CODEC_H264 : CSI_H264_CODEC_MJPEG;
}

void csi_h264_ctrl_on_mode_reset(csi_h264_ctrl_t *c)
{
    if (!c) {
        return;
    }
    c->need_idr = true;
    c->active_w = 0u;
    c->active_h = 0u;
    c->frames_since_idr = 0u;
    c->oom_cooldown_frames = 0u;
    c->consec_failures = 0u;
}

bool csi_h264_ctrl_begin_frame(csi_h264_ctrl_t *c, uint32_t width, uint32_t height,
                               bool *out_use_h264, bool *out_force_idr)
{
    if (!c) {
        if (out_use_h264) {
            *out_use_h264 = false;
        }
        if (out_force_idr) {
            *out_force_idr = false;
        }
        return false;
    }

    uint32_t req = __atomic_load_n(&c->requested_codec, __ATOMIC_ACQUIRE);
    uint32_t req_epoch = __atomic_load_n(&c->requested_epoch, __ATOMIC_ACQUIRE);
    if (req_epoch != c->seen_req_epoch) {
        c->seen_req_epoch = req_epoch;
        c->oom_cooldown_frames = 0u;
        c->consec_failures = 0u;
    }

    bool cooling_down = false;
    if (req != (uint32_t)CSI_H264_CODEC_H264) {
        c->oom_cooldown_frames = 0u;
        c->consec_failures = 0u;
    } else if (c->oom_cooldown_frames > 0u) {
        c->oom_cooldown_frames--;
        cooling_down = true;
    }

    uint32_t prev_active = __atomic_load_n(&c->active_codec, __ATOMIC_RELAXED);
    bool res_ok = csi_h264_check_resolution(width, height);
    bool h264_capable = __atomic_load_n(&c->h264_capable, __ATOMIC_ACQUIRE);
    bool want_h264 = h264_capable && res_ok &&
                     (req == (uint32_t)CSI_H264_CODEC_H264) && !cooling_down;

    if (!want_h264) {
        bool switched = (prev_active != (uint32_t)CSI_H264_CODEC_MJPEG);
        if (switched) {
            __atomic_store_n(&c->active_codec, (uint32_t)CSI_H264_CODEC_MJPEG, __ATOMIC_RELEASE);
            c->switches_to_mjpeg++;
        }
        (void)__atomic_exchange_n(&c->keyframe_req, 0u, __ATOMIC_ACQ_REL);
        c->need_idr = false;
        c->frames_since_idr = 0u;
        c->consec_failures = 0u;
        if (out_use_h264) {
            *out_use_h264 = false;
        }
        if (out_force_idr) {
            *out_force_idr = false;
        }
        return switched;
    }

    bool switched = (prev_active != (uint32_t)CSI_H264_CODEC_H264);
    if (switched) {
        __atomic_store_n(&c->active_codec, (uint32_t)CSI_H264_CODEC_H264, __ATOMIC_RELEASE);
        c->switches_to_h264++;
        c->need_idr = true;
        c->frames_since_idr = 0u;
    }

    if (c->active_w != width || c->active_h != height) {
        c->need_idr = true;
        c->frames_since_idr = 0u;
        c->active_w = width;
        c->active_h = height;
    }

    bool ext_req = (__atomic_exchange_n(&c->keyframe_req, 0u, __ATOMIC_ACQ_REL) != 0u);
    if (ext_req) {
        c->keyframe_reqs_applied++;
    }

    bool gop_due = (c->frames_since_idr >= (uint32_t)c->gop);
    bool force_idr = c->need_idr || ext_req || gop_due;
    c->need_idr = force_idr;

    if (out_use_h264) {
        *out_use_h264 = true;
    }
    if (out_force_idr) {
        *out_force_idr = force_idr;
    }
    return switched;
}

void csi_h264_ctrl_end_frame(csi_h264_ctrl_t *c, csi_h264_frame_outcome_t outcome,
                             bool actual_is_idr)
{
    if (!c) {
        return;
    }
    switch (outcome) {
    case CSI_H264_OUTCOME_PUBLISHED:
        if (actual_is_idr) {
            c->need_idr = false;
            c->frames_since_idr = 1u;
            c->consec_failures = 0u;
            c->idr_frames++;
            break;
        }
        if (!c->need_idr) {
            c->frames_since_idr++;
            c->consec_failures = 0u;
            c->p_frames++;
            break;
        }
        /* Unexpected P when IDR was required: treat as encode error below. */
        /* fall through */
    case CSI_H264_OUTCOME_OVERSIZE:
    case CSI_H264_OUTCOME_ENCODE_ERR:
        c->need_idr = true;
        c->frames_since_idr = 0u;
        c->self_idr_recoveries++;
        c->consec_failures++;
        if (c->consec_failures >= CSI_H264_MAX_CONSEC_FAILURES) {
            c->consec_failures = 0u;
            if (__atomic_load_n(&c->active_codec, __ATOMIC_RELAXED) != (uint32_t)CSI_H264_CODEC_MJPEG) {
                __atomic_store_n(&c->active_codec, (uint32_t)CSI_H264_CODEC_MJPEG, __ATOMIC_RELEASE);
                c->switches_to_mjpeg++;
            }
            (void)__atomic_exchange_n(&c->keyframe_req, 0u, __ATOMIC_ACQ_REL);
            c->oom_cooldown_frames = CSI_H264_OOM_RETRY_FRAMES;
        }
        break;
    case CSI_H264_OUTCOME_PUBLISH_BUSY:
        c->need_idr = true;
        c->frames_since_idr = 0u;
        c->self_idr_recoveries++;
        break;
    case CSI_H264_OUTCOME_FALLBACK_MJPEG:
        if (__atomic_load_n(&c->active_codec, __ATOMIC_RELAXED) != (uint32_t)CSI_H264_CODEC_MJPEG) {
            __atomic_store_n(&c->active_codec, (uint32_t)CSI_H264_CODEC_MJPEG, __ATOMIC_RELEASE);
            c->switches_to_mjpeg++;
        }
        (void)__atomic_exchange_n(&c->keyframe_req, 0u, __ATOMIC_ACQ_REL);
        c->need_idr = true;
        c->frames_since_idr = 0u;
        c->consec_failures = 0u;
        c->oom_cooldown_frames = CSI_H264_OOM_RETRY_FRAMES;
        break;
    }
}
