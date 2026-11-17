#include "csi_h264.h"

#include "sdkconfig.h"

#if defined(CONFIG_CSI_H264_ENABLE) && CONFIG_CSI_H264_ENABLE

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "csi_h264_logic.h"
#include "driver/ppa.h"
#include "esp_cache.h"
#include "esp_h264_alloc.h"
#include "esp_h264_enc_single_hw.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_private/esp_cache_private.h"
#include "esp_timer.h"

static const char *TAG = "csi_h264";

struct csi_h264_t {
    csi_h264_config_t cfg;

    // PPA + PSRAM buffers (persistent if prealloc_buffers, on-demand otherwise).
    ppa_client_handle_t ppa_srm;
    uint8_t *yuv_buf;
    size_t yuv_buf_bytes;
    uint8_t *out_buf;
    size_t out_buf_bytes;

    // HW H.264 encoder bound to (enc_w x enc_h).
    esp_h264_enc_handle_t enc;
    esp_h264_enc_param_hw_handle_t param_hd;
    uint32_t enc_w;
    uint32_t enc_h;
    uint32_t zeroed_w;
    uint32_t zeroed_h;
};

static esp_err_t ensure_buffers(struct csi_h264_t *h)
{
    if (h->ppa_srm && h->yuv_buf && h->out_buf) {
        return ESP_OK;
    }

    size_t int_before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t psram_before = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

    if (!h->ppa_srm) {
        ppa_client_config_t ppa_cfg = {
            .oper_type = PPA_OPERATION_SRM,
            .max_pending_trans_num = 1,
            .data_burst_length = PPA_DATA_BURST_LENGTH_128,
        };
        esp_err_t err = ppa_register_client(&ppa_cfg, &h->ppa_srm);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "ppa_register_client failed: %s", esp_err_to_name(err));
            return err;
        }
    }

    if (!h->yuv_buf) {
        size_t req_yuv = csi_h264_calc_yuv_bytes(h->cfg.max_width, h->cfg.max_height);
        if (req_yuv == 0u || req_yuv > (size_t)UINT32_MAX) {
            return ESP_ERR_INVALID_ARG;
        }
        uint32_t actual_yuv = 0u;
        h->yuv_buf = (uint8_t *)esp_h264_aligned_calloc(
            CSI_H264_DMA_ALIGN, 1u, (uint32_t)req_yuv, &actual_yuv, ESP_H264_MEM_SPIRAM);
        if (!h->yuv_buf || actual_yuv < req_yuv) {
            ESP_LOGE(TAG, "PSRAM alloc for YUV420 buffer (%u B) failed (largest=%u B)",
                     (unsigned)req_yuv,
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
            return ESP_ERR_NO_MEM;
        }
        h->yuv_buf_bytes = (size_t)actual_yuv;
        h->zeroed_w = 0u;
        h->zeroed_h = 0u;
    }

    if (!h->out_buf) {
        size_t req_out = csi_h264_calc_output_buf_size(h->cfg.max_output_bytes);
        if (req_out == 0u || req_out > (size_t)UINT32_MAX) {
            return ESP_ERR_INVALID_ARG;
        }
        uint32_t actual_out = 0u;
        h->out_buf = (uint8_t *)esp_h264_aligned_calloc(
            CSI_H264_DMA_ALIGN, 1u, (uint32_t)req_out, &actual_out, ESP_H264_MEM_SPIRAM);
        if (!h->out_buf || actual_out < req_out) {
            ESP_LOGE(TAG, "PSRAM alloc for H.264 out buffer (%u B) failed (largest=%u B)",
                     (unsigned)req_out,
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
            return ESP_ERR_NO_MEM;
        }
        h->out_buf_bytes = (size_t)actual_out;
    }

    size_t int_after = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t psram_after = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "H.264 buffers ready (%s): yuv=%u B @ %p, out=%u B @ %p | "
                  "int_delta=-%u B, psram_delta=-%u B, largest_int=%u B, largest_psram=%u B",
             h->cfg.prealloc_buffers ? "prealloc" : "on-switch",
             (unsigned)h->yuv_buf_bytes, (void *)h->yuv_buf,
             (unsigned)h->out_buf_bytes, (void *)h->out_buf,
             (unsigned)((int_before > int_after) ? (int_before - int_after) : 0u),
             (unsigned)((psram_before > psram_after) ? (psram_before - psram_after) : 0u),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
    return ESP_OK;
}

static void free_buffers(struct csi_h264_t *h)
{
    if (h->yuv_buf) {
        esp_h264_free(h->yuv_buf);
        h->yuv_buf = NULL;
        h->yuv_buf_bytes = 0u;
        h->zeroed_w = 0u;
        h->zeroed_h = 0u;
    }
    if (h->out_buf) {
        esp_h264_free(h->out_buf);
        h->out_buf = NULL;
        h->out_buf_bytes = 0u;
    }
    if (h->ppa_srm) {
        (void)ppa_unregister_client(h->ppa_srm);
        h->ppa_srm = NULL;
    }
}

static void close_encoder(struct csi_h264_t *h)
{
    if (h->enc) {
        (void)esp_h264_enc_close(h->enc);
        (void)esp_h264_enc_del(h->enc);
        h->enc = NULL;
        h->param_hd = NULL;
    }
    h->enc_w = 0u;
    h->enc_h = 0u;
}

static esp_err_t ensure_zero_padding(struct csi_h264_t *h, uint32_t width, uint32_t height)
{
    if (h->zeroed_w == width && h->zeroed_h == height) {
        return ESP_OK;
    }
    size_t enc_yuv_bytes = csi_h264_calc_yuv_bytes(width, height);
    size_t active_bytes = csi_h264_calc_ppa_active_bytes(width, height);
    if (enc_yuv_bytes == 0u || enc_yuv_bytes > h->yuv_buf_bytes || active_bytes > enc_yuv_bytes) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (enc_yuv_bytes > active_bytes) {
        // Zero trailing padding rows (e.g. lines 1080..1087 on 1920x1080) and flush C2M
        // before PPA runs M2C invalidation on [0..active_bytes).
        size_t pad_bytes = enc_yuv_bytes - active_bytes;
        memset(h->yuv_buf + active_bytes, 0, pad_bytes);
        size_t sync_len = csi_h264_align_up(pad_bytes, CSI_H264_DMA_ALIGN);
        size_t start_off = active_bytes - (active_bytes % CSI_H264_DMA_ALIGN);
        if (start_off + sync_len <= h->yuv_buf_bytes) {
            (void)esp_cache_msync(h->yuv_buf + start_off, sync_len, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
        } else {
            (void)esp_cache_msync(h->yuv_buf + active_bytes, pad_bytes,
                                  ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
        }
    }
    h->zeroed_w = width;
    h->zeroed_h = height;
    return ESP_OK;
}

static esp_err_t ensure_encoder(struct csi_h264_t *h, uint32_t width, uint32_t height)
{
    if (h->enc && h->enc_w == width && h->enc_h == height) {
        return ESP_OK;
    }

    close_encoder(h);

    uint8_t fps = csi_h264_calc_target_fps(width, height);
    uint64_t pixels = (uint64_t)width * (uint64_t)height;
    uint32_t bitrate = (pixels > (uint64_t)(1280u * 720u))
                           ? h->cfg.bitrate_1080p_bps
                           : h->cfg.bitrate_720p_bps;

    if (!csi_h264_validate_enc_params(width, height, h->cfg.gop, fps,
                                      bitrate, h->cfg.qp_min, h->cfg.qp_max)) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t int_before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t psram_before = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

    esp_h264_enc_cfg_hw_t enc_cfg = {
        .pic_type = ESP_H264_RAW_FMT_O_UYY_E_VYY,
        .gop = h->cfg.gop,
        .fps = fps,
        .res = {
            .width = (uint16_t)width,
            .height = (uint16_t)height,
        },
        .rc = {
            .bitrate = bitrate,
            .qp_min = h->cfg.qp_min,
            .qp_max = h->cfg.qp_max,
        },
    };

    esp_h264_err_t h_err = esp_h264_enc_hw_new(&enc_cfg, &h->enc);
    if (h_err != ESP_H264_ERR_OK || !h->enc) {
        ESP_LOGE(TAG, "esp_h264_enc_hw_new(%" PRIu32 "x%" PRIu32 ") failed: %d "
                      "(free_int=%u B, largest_int=%u B)",
                 width, height, (int)h_err,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        h->enc = NULL;
        return (h_err == ESP_H264_ERR_MEM) ? ESP_ERR_NO_MEM : ESP_FAIL;
    }

    h_err = esp_h264_enc_open(h->enc);
    if (h_err != ESP_H264_ERR_OK) {
        ESP_LOGE(TAG, "esp_h264_enc_open failed: %d", (int)h_err);
        (void)esp_h264_enc_del(h->enc);
        h->enc = NULL;
        return (h_err == ESP_H264_ERR_MEM) ? ESP_ERR_NO_MEM : ESP_FAIL;
    }

    h_err = esp_h264_enc_hw_get_param_hd(h->enc, &h->param_hd);
    if (h_err != ESP_H264_ERR_OK || !h->param_hd) {
        ESP_LOGE(TAG, "esp_h264_enc_hw_get_param_hd failed: %d", (int)h_err);
        close_encoder(h);
        return ESP_FAIL;
    }

    h->enc_w = width;
    h->enc_h = height;

    size_t int_after = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t psram_after = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "Opened HW H.264 encoder %" PRIu32 "x%" PRIu32
                  " (gop=%u fps=%u bitrate=%" PRIu32 " qp=%u..%u): "
                  "int_delta=-%u B, psram_delta=-%u B, free_int=%u B (min=%u B, largest=%u B), free_psram=%u B",
             width, height, (unsigned)h->cfg.gop, (unsigned)fps, bitrate,
             (unsigned)h->cfg.qp_min, (unsigned)h->cfg.qp_max,
             (unsigned)((int_before > int_after) ? (int_before - int_after) : 0u),
             (unsigned)((psram_before > psram_after) ? (psram_before - psram_after) : 0u),
             (unsigned)int_after,
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)psram_after);

    return ESP_OK;
}

esp_err_t csi_h264_create(const csi_h264_config_t *cfg, csi_h264_handle_t *out)
{
    if (!cfg || !out) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = NULL;

    if (!csi_h264_validate_enc_params(cfg->max_width, cfg->max_height, cfg->gop,
                                      csi_h264_calc_target_fps(cfg->max_width, cfg->max_height),
                                      cfg->bitrate_1080p_bps, cfg->qp_min, cfg->qp_max) ||
        cfg->bitrate_720p_bps == 0u ||
        csi_h264_calc_output_buf_size(cfg->max_output_bytes) == 0u) {
        ESP_LOGE(TAG, "Invalid csi_h264 config: max=%" PRIu32 "x%" PRIu32
                      " out=%u gop=%u qp=%u..%u",
                 cfg->max_width, cfg->max_height, (unsigned)cfg->max_output_bytes,
                 (unsigned)cfg->gop, (unsigned)cfg->qp_min, (unsigned)cfg->qp_max);
        return ESP_ERR_INVALID_ARG;
    }

    struct csi_h264_t *h = (struct csi_h264_t *)calloc(1u, sizeof(*h));
    if (!h) {
        return ESP_ERR_NO_MEM;
    }
    h->cfg = *cfg;

    if (cfg->prealloc_buffers) {
        esp_err_t err = ensure_buffers(h);
        if (err != ESP_OK) {
            (void)csi_h264_destroy(h);
            return err;
        }
    }

    *out = h;
    return ESP_OK;
}

esp_err_t csi_h264_ppa_rgb_to_yuv(csi_h264_handle_t h,
                                  const uint8_t *bgr24_in, size_t in_bytes,
                                  uint32_t width, uint32_t height,
                                  uint32_t *out_ppa_us)
{
    if (out_ppa_us) {
        *out_ppa_us = 0u;
    }
    if (!h || !bgr24_in || !csi_h264_check_resolution(width, height) ||
        width > h->cfg.max_width || height > h->cfg.max_height) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t expected_in = (size_t)width * (size_t)height * 3u;
    if (in_bytes != expected_in) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = ensure_buffers(h);
    if (err != ESP_OK) {
        return err;
    }

    err = ensure_zero_padding(h, width, height);
    if (err != ESP_OK) {
        return err;
    }

    size_t enc_yuv_bytes = csi_h264_calc_yuv_bytes(width, height);
    uint32_t aligned_w = (uint32_t)csi_h264_align_up((size_t)width, CSI_H264_ALIGN_DIM);
    if (enc_yuv_bytes == 0u || enc_yuv_bytes > h->yuv_buf_bytes || enc_yuv_bytes > (size_t)UINT32_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }

    int64_t t0 = esp_timer_get_time();
    ppa_srm_oper_config_t srm_cfg = {
        .in = {
            .buffer = bgr24_in,
            .pic_w = width,
            .pic_h = height,
            .block_w = width,
            .block_h = height,
            .block_offset_x = 0,
            .block_offset_y = 0,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB888,
        },
        .out = {
            .buffer = h->yuv_buf,
            .buffer_size = (uint32_t)enc_yuv_bytes,
            .pic_w = aligned_w,
            .pic_h = height,
            .block_offset_x = 0,
            .block_offset_y = 0,
            .srm_cm = PPA_SRM_COLOR_MODE_YUV420,
            .yuv_range = PPA_COLOR_RANGE_LIMIT,
            .yuv_std = PPA_COLOR_CONV_STD_RGB_YUV_BT601,
        },
        .rotation_angle = PPA_SRM_ROTATION_ANGLE_0,
        .scale_x = 1.0f,
        .scale_y = 1.0f,
        .mirror_x = false,
        .mirror_y = false,
        .rgb_swap = false,
        .byte_swap = false,
        .alpha_update_mode = PPA_ALPHA_NO_CHANGE,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };

    err = ppa_do_scale_rotate_mirror(h->ppa_srm, &srm_cfg);
    int64_t t1 = esp_timer_get_time();
    if (out_ppa_us) {
        *out_ppa_us = (t1 > t0) ? (uint32_t)(t1 - t0) : 0u;
    }
    return err;
}

esp_err_t csi_h264_encode_prepared(csi_h264_handle_t h,
                                   uint32_t width, uint32_t height,
                                   bool force_idr, uint32_t pts_ms,
                                   csi_h264_frame_out_t *out)
{
    if (!h || !out || !csi_h264_check_resolution(width, height) ||
        width > h->cfg.max_width || height > h->cfg.max_height) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));

    if (!h->yuv_buf || !h->out_buf) {
        return ESP_ERR_INVALID_STATE;
    }

    size_t enc_yuv_bytes = csi_h264_calc_yuv_bytes(width, height);
    if (enc_yuv_bytes == 0u || enc_yuv_bytes > h->yuv_buf_bytes) {
        return ESP_ERR_INVALID_SIZE;
    }

    esp_err_t err = ensure_encoder(h, width, height);
    if (err != ESP_OK) {
        return err;
    }

    if (force_idr) {
        esp_h264_err_t idr_err = esp_h264_enc_force_idr(&h->param_hd->base);
        if (idr_err != ESP_H264_ERR_OK) {
            ESP_LOGW(TAG, "esp_h264_enc_force_idr failed: %d", (int)idr_err);
        }
    }

    esp_h264_enc_in_frame_t in_frame = {
        .raw_data = {
            .buffer = h->yuv_buf,
            .len = (uint32_t)enc_yuv_bytes,
        },
        .pts = pts_ms,
    };
    esp_h264_enc_out_frame_t out_frame = {
        .raw_data = {
            .buffer = h->out_buf,
            .len = (uint32_t)h->out_buf_bytes,
        },
    };

    int64_t t0 = esp_timer_get_time();
    esp_h264_err_t h_err = esp_h264_enc_process(h->enc, &in_frame, &out_frame);
    int64_t t1 = esp_timer_get_time();
    out->enc_us = (t1 > t0) ? (uint32_t)(t1 - t0) : 0u;

    if (h_err == ESP_H264_ERR_OVERFLOW) {
        out->len = (size_t)out_frame.length;
        return CSI_H264_ERR_OUTPUT_OVERSIZE;
    }
    if (h_err != ESP_H264_ERR_OK) {
        ESP_LOGW(TAG, "esp_h264_enc_process failed: %d", (int)h_err);
        return (h_err == ESP_H264_ERR_TIMEOUT) ? ESP_ERR_TIMEOUT : ESP_FAIL;
    }

    bool is_key = (out_frame.frame_type == ESP_H264_FRAME_TYPE_IDR ||
                   out_frame.frame_type == ESP_H264_FRAME_TYPE_I);

    csi_h264_frame_err_t ferr = csi_h264_check_frame(
        h->out_buf, (size_t)out_frame.length, is_key, h->cfg.max_output_bytes);
    if (ferr == CSI_H264_FRAME_ERR_OVERSIZE) {
        out->len = (size_t)out_frame.length;
        return CSI_H264_ERR_OUTPUT_OVERSIZE;
    }
    if (ferr != CSI_H264_FRAME_OK) {
        ESP_LOGW(TAG, "Malformed H.264 output (err=%d len=%" PRIu32 " key=%d head=%02x%02x%02x%02x %02x)",
                 (int)ferr, out_frame.length, (int)is_key,
                 out_frame.length > 0 ? h->out_buf[0] : 0u,
                 out_frame.length > 1 ? h->out_buf[1] : 0u,
                 out_frame.length > 2 ? h->out_buf[2] : 0u,
                 out_frame.length > 3 ? h->out_buf[3] : 0u,
                 out_frame.length > 4 ? h->out_buf[4] : 0u);
        return ESP_ERR_INVALID_RESPONSE;
    }

    out->data = h->out_buf;
    out->len = (size_t)out_frame.length;
    out->is_key = is_key;
    return ESP_OK;
}

void csi_h264_release_stream(csi_h264_handle_t h)
{
    if (!h) {
        return;
    }
    bool was_active = (h->enc != NULL) || (!h->cfg.prealloc_buffers && (h->yuv_buf != NULL || h->ppa_srm != NULL));
    close_encoder(h);
    if (!h->cfg.prealloc_buffers) {
        free_buffers(h);
    }
    if (was_active) {
        ESP_LOGI(TAG, "Released H.264 stream (%s): free_int=%u B, largest_int=%u B, free_psram=%u B, largest_psram=%u B",
                 h->cfg.prealloc_buffers ? "kept-psram" : "freed-all",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
    }
}

size_t csi_h264_yuv_capacity(csi_h264_handle_t h)
{
    return h ? h->yuv_buf_bytes : 0u;
}

size_t csi_h264_output_capacity(csi_h264_handle_t h)
{
    return h ? h->out_buf_bytes : 0u;
}

bool csi_h264_is_stream_active(csi_h264_handle_t h)
{
    return h ? (h->enc != NULL) : false;
}

esp_err_t csi_h264_destroy(csi_h264_handle_t h)
{
    if (!h) {
        return ESP_OK;
    }
    close_encoder(h);
    free_buffers(h);
    free(h);
    return ESP_OK;
}

#endif // CONFIG_CSI_H264_ENABLE
