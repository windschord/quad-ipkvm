#include "csi_jpeg.h"

#include <inttypes.h>
#include <stdlib.h>
#include "csi_capture_logic.h"
#include "driver/jpeg_encode.h"
#include "driver/ppa.h"
#include "esp_cache.h"
#include "esp_private/esp_cache_private.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "csi_jpeg";

#define CSI_JPEG_MIN_BUF_BYTES    4096u
#define CSI_JPEG_DEFAULT_TIMEOUT  100
#define CSI_JPEG_PPA_ALIGN        128u

struct csi_jpeg_t {
    jpeg_encoder_handle_t eng;
    ppa_client_handle_t ppa_srm;
    uint8_t *out_buf;
    size_t max_output_bytes;
    size_t out_buf_bytes;
    size_t sentinel_bytes;
    uint8_t *ppa_buf;
    size_t ppa_buf_bytes;
    size_t ppa_valid_bytes;
    uint32_t ppa_valid_w;
    uint32_t ppa_valid_h;
    bool logged_ppa_sample;
    uint8_t quality;
};

static esp_err_t validate_jpeg_bitstream(const uint8_t *buf, size_t cap, uint32_t encoded_bytes)
{
    csi_jpeg_bitstream_result_t r = csi_jpeg_validate_bitstream(buf, cap, (size_t)encoded_bytes);
    if (r == CSI_JPEG_BITSTREAM_ERR_OVERSIZE) {
        ESP_LOGE(TAG, "JPEG output exceeded buffer capacity: len=%" PRIu32 " cap=%u",
                 encoded_bytes, (unsigned)cap);
        return CSI_JPEG_ERR_OUTPUT_OVERSIZE;
    }
    if (r != CSI_JPEG_BITSTREAM_OK) {
        ESP_LOGE(TAG, "Invalid JPEG output: len=%" PRIu32 " head=%02x%02x tail=%02x%02x",
                 encoded_bytes,
                 (buf && encoded_bytes >= 1u) ? buf[0] : 0u,
                 (buf && encoded_bytes >= 2u) ? buf[1] : 0u,
                 (buf && encoded_bytes >= 2u) ? buf[encoded_bytes - 2u] : 0u,
                 (buf && encoded_bytes >= 1u) ? buf[encoded_bytes - 1u] : 0u);
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

esp_err_t csi_jpeg_open(const csi_jpeg_config_t *cfg, csi_jpeg_handle_t *out)
{
    if (!cfg || !out) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = NULL;

    if (cfg->quality < 1u || cfg->quality > 100u ||
        cfg->max_output_bytes < CSI_JPEG_MIN_BUF_BYTES ||
        cfg->max_output_bytes > (size_t)UINT32_MAX - CSI_JPEG_OVERFLOW_GUARD_BYTES) {
        ESP_LOGE(TAG, "Invalid JPEG config: quality=%u max_output_bytes=%u",
                 (unsigned)cfg->quality, (unsigned)cfg->max_output_bytes);
        return ESP_ERR_INVALID_ARG;
    }

    size_t ppa_bytes = 0u;
    if (cfg->enable_uyvy_ppa) {
        size_t max_raw = 0u;
        if (csi_capture_check_format_geometry(cfg->max_width, cfg->max_height, CSI_PIXEL_FORMAT_UYVY,
                                              cfg->max_width, cfg->max_height, SIZE_MAX, &max_raw) != CSI_GEOM_OK) {
            ESP_LOGE(TAG, "Invalid UYVY PPA max geometry %" PRIu32 "x%" PRIu32,
                     cfg->max_width, cfg->max_height);
            return ESP_ERR_INVALID_ARG;
        }
        ppa_bytes = csi_capture_round_up(max_raw, CSI_JPEG_PPA_ALIGN);
        if (ppa_bytes == 0u || ppa_bytes > (size_t)UINT32_MAX) {
            return ESP_ERR_INVALID_ARG;
        }
    }

    struct csi_jpeg_t *h = calloc(1, sizeof(*h));
    if (!h) {
        return ESP_ERR_NO_MEM;
    }
    h->max_output_bytes = cfg->max_output_bytes;
    h->quality = cfg->quality;

    // Source: ESP-IDF v6.0.1 components/esp_driver_jpeg/jpeg_encode.c:102-168.
    jpeg_encode_engine_cfg_t eng_cfg = {
        .intr_priority = 0,
        .timeout_ms = (cfg->timeout_ms > 0) ? cfg->timeout_ms : CSI_JPEG_DEFAULT_TIMEOUT,
    };
    esp_err_t err = jpeg_new_encoder_engine(&eng_cfg, &h->eng);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "jpeg_new_encoder_engine failed: %s", esp_err_to_name(err));
        free(h);
        return err;
    }

    // Source: ESP-IDF v6.0.1 components/esp_driver_jpeg/jpeg_encode.c:177, 380-396.
    // JPEG_ENC_ALLOC_OUTPUT_BUFFER aligns both base address and buffer size to the PSRAM
    // data-cache line size so DMA2D writes and M2C cache invalidation cannot fault or corrupt neighbors.
    // Allocate CSI_JPEG_OVERFLOW_GUARD_BYTES past cfg->max_output_bytes so borderline oversize
    // frames still finish DMA2D (caught cleanly by validate_jpeg_bitstream) and larger bursts
    // overwrite the trailing cache-line sentinel before hitting JPEG_DMA2D_RX_DESC_EMPTY.
    size_t cache_align = 0u;
    (void)esp_cache_get_alignment(MALLOC_CAP_SPIRAM, &cache_align);
    if (cache_align == 0u || cache_align > CSI_JPEG_OVERFLOW_GUARD_BYTES) {
        cache_align = CSI_JPEG_PPA_ALIGN;
    }
    h->sentinel_bytes = cache_align;

    size_t req_dma_bytes = cfg->max_output_bytes + CSI_JPEG_OVERFLOW_GUARD_BYTES;
    jpeg_encode_memory_alloc_cfg_t mem_cfg = {
        .buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER,
    };
    size_t alloc_bytes = 0;
    h->out_buf = (uint8_t *)jpeg_alloc_encoder_mem(req_dma_bytes, &mem_cfg, &alloc_bytes);
    if (!h->out_buf || alloc_bytes < req_dma_bytes || alloc_bytes > (size_t)UINT32_MAX) {
        ESP_LOGE(TAG, "jpeg_alloc_encoder_mem(%u) failed", (unsigned)req_dma_bytes);
        (void)csi_jpeg_close(h);
        return ESP_ERR_NO_MEM;
    }
    h->out_buf_bytes = alloc_bytes;

    if (cfg->enable_uyvy_ppa) {
        // Source: ESP-IDF v6.0.1 components/esp_driver_ppa/src/ppa_core.c, ppa_srm.c.
        ppa_client_config_t ppa_cfg = {
            .oper_type = PPA_OPERATION_SRM,
            .max_pending_trans_num = 1,
            .data_burst_length = PPA_DATA_BURST_LENGTH_128,
        };
        err = ppa_register_client(&ppa_cfg, &h->ppa_srm);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "ppa_register_client failed: %s", esp_err_to_name(err));
            (void)csi_jpeg_close(h);
            return err;
        }
        h->ppa_buf = (uint8_t *)heap_caps_aligned_calloc(
            CSI_JPEG_PPA_ALIGN, 1, ppa_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
        if (!h->ppa_buf) {
            ESP_LOGE(TAG, "PSRAM alloc for UYVY PPA buffer (%u B) failed", (unsigned)ppa_bytes);
            (void)csi_jpeg_close(h);
            return ESP_ERR_NO_MEM;
        }
        h->ppa_buf_bytes = ppa_bytes;
        (void)esp_cache_msync(h->ppa_buf, h->ppa_buf_bytes,
                              ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_INVALIDATE);
    }

    ESP_LOGI(TAG, "Opened HW JPEG encoder: quality=%u, %s, output_buf=%u bytes @ %p, ppa_buf=%u bytes @ %p",
             (unsigned)h->quality, cfg->enable_uyvy_ppa ? "UYVY->PPA->YUV422" : "RGB888->YUV420",
             (unsigned)h->out_buf_bytes, (void *)h->out_buf,
             (unsigned)h->ppa_buf_bytes, (void *)h->ppa_buf);
    *out = h;
    return ESP_OK;
}

static esp_err_t run_jpeg_encoder(csi_jpeg_handle_t h,
                                const jpeg_encode_cfg_t *enc_cfg,
                                const uint8_t *in_buf, uint32_t in_bytes,
                                const uint8_t **out_jpeg, size_t *out_len)
{
    // Arm the trailing cache-line sentinel so even a hard DMA2D RX exhaustion
    // (JPEG_DMA2D_RX_DESC_EMPTY -> ESP_ERR_INVALID_STATE) is distinguishable from
    // encoder status or alignment errors.
    uint8_t *sentinel_ptr = h->out_buf + (h->out_buf_bytes - h->sentinel_bytes);
    csi_jpeg_arm_overflow_sentinel(sentinel_ptr, h->sentinel_bytes);
    (void)esp_cache_msync(sentinel_ptr, h->sentinel_bytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M);

    uint32_t encoded_bytes = 0;
    esp_err_t err = jpeg_encoder_process(h->eng, enc_cfg,
                                         in_buf, in_bytes,
                                         h->out_buf, (uint32_t)h->out_buf_bytes,
                                         &encoded_bytes);
    if (err != ESP_OK) {
        if (err == ESP_ERR_INVALID_STATE) {
            (void)esp_cache_msync(sentinel_ptr, h->sentinel_bytes, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
            if (csi_jpeg_overflow_sentinel_tripped(sentinel_ptr, h->sentinel_bytes)) {
                ESP_LOGW(TAG, "JPEG DMA2D RX buffer exhausted (cap=%u, dma_buf=%u); reporting CSI_JPEG_ERR_OUTPUT_OVERSIZE",
                         (unsigned)h->max_output_bytes, (unsigned)h->out_buf_bytes);
                return CSI_JPEG_ERR_OUTPUT_OVERSIZE;
            }
        }
        return err;
    }

    err = validate_jpeg_bitstream(h->out_buf, h->max_output_bytes, encoded_bytes);
    if (err != ESP_OK) {
        return err;
    }

    *out_jpeg = h->out_buf;
    *out_len = (size_t)encoded_bytes;
    return ESP_OK;
}

esp_err_t csi_jpeg_encode(csi_jpeg_handle_t h,
                          const uint8_t *bgr24_in, size_t in_bytes,
                          uint32_t width, uint32_t height,
                          const uint8_t **out_jpeg, size_t *out_len)
{
    if (!h || !bgr24_in || !out_jpeg || !out_len) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_jpeg = NULL;
    *out_len = 0;

    size_t expected_in = 0;
    if (csi_capture_check_geometry(width, height, 24u, width, height, in_bytes, &expected_in) != CSI_GEOM_OK ||
        in_bytes != expected_in || in_bytes > (size_t)UINT32_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    // Note on field names: driver/jpeg_encode.h:22-23 has swapped Doxygen comments on
    // height and width, whereas jpeg_encode.c:237-244 maps encode_cfg->width -> origin_h ->
    // jpeg_ll_set_picture_width() and encode_cfg->height -> origin_v -> jpeg_ll_set_picture_height().
    // Source: ESP-IDF v6.0.1 components/esp_driver_jpeg/jpeg_encode.c:200-249, 286, 319-320.
    // - JPEG_ENCODE_IN_FORMAT_RGB888 == ESP_COLOR_FOURCC_BGR24 (jpeg_types.h:58), matching
    //   the B,G,R byte order produced by CSI DMA on ESP32-P4 rev v1.3.
    // - jpeg_encoder_process() executes C2M cache sync on bgr24_in before DMA2D TX and
    //   M2C cache invalidation on the compressed bitstream after DMA2D RX EOF.
    jpeg_encode_cfg_t enc_cfg = {
        .width = width,
        .height = height,
        .src_type = JPEG_ENCODE_IN_FORMAT_RGB888,
        .sub_sample = JPEG_DOWN_SAMPLING_YUV420,
        .image_quality = h->quality,
        .pixel_reverse = false,
    };

    return run_jpeg_encoder(h, &enc_cfg, bgr24_in, (uint32_t)in_bytes, out_jpeg, out_len);
}

esp_err_t csi_jpeg_ppa_swap_uyvy(csi_jpeg_handle_t h,
                                 const uint8_t *uyvy_dma_in, size_t in_bytes,
                                 uint32_t width, uint32_t height)
{
    if (!h || !h->ppa_srm || !h->ppa_buf || !uyvy_dma_in) {
        return ESP_ERR_INVALID_STATE;
    }

    size_t expected_in = 0u;
    if (csi_capture_check_format_geometry(width, height, CSI_PIXEL_FORMAT_UYVY,
                                          width, height, in_bytes, &expected_in) != CSI_GEOM_OK ||
        in_bytes != expected_in) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t aligned_out = csi_capture_round_up(in_bytes, CSI_JPEG_PPA_ALIGN);
    if (aligned_out == 0u || aligned_out > h->ppa_buf_bytes) {
        return ESP_ERR_INVALID_SIZE;
    }

    // Treat every 2-pixel 32-bit UYVY word (4 bytes) as 1 ARGB8888 pixel with rgb_swap = true:
    // ppa_ll_srm_enable_rx_rgb_swap() reverses ARGB -> BGRA ([b0,b1,b2,b3] -> [b3,b2,b1,b0]),
    // restoring CSI-DMA-reversed [Y1,V0,Y0,U0] back to wire order [U0,Y0,V0,Y1].
    // Source: ESP-IDF v6.0.1 hal/ppa_ll.h:438-446, ppa_srm.c:134-149, 259-270.
    uint32_t argb_w = width / 2u;
    ppa_srm_oper_config_t srm_cfg = {
        .in = {
            .buffer = uyvy_dma_in,
            .pic_w = argb_w,
            .pic_h = height,
            .block_w = argb_w,
            .block_h = height,
            .block_offset_x = 0,
            .block_offset_y = 0,
            .srm_cm = PPA_SRM_COLOR_MODE_ARGB8888,
        },
        .out = {
            .buffer = h->ppa_buf,
            .buffer_size = (uint32_t)aligned_out,
            .pic_w = argb_w,
            .pic_h = height,
            .block_offset_x = 0,
            .block_offset_y = 0,
            .srm_cm = PPA_SRM_COLOR_MODE_ARGB8888,
        },
        .rotation_angle = PPA_SRM_ROTATION_ANGLE_0,
        .scale_x = 1.0f,
        .scale_y = 1.0f,
        .mirror_x = false,
        .mirror_y = false,
        .rgb_swap = true,
        .byte_swap = false,
        .alpha_update_mode = PPA_ALPHA_NO_CHANGE,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };

    esp_err_t err = ppa_do_scale_rotate_mirror(h->ppa_srm, &srm_cfg);
    if (err != ESP_OK) {
        return err;
    }

    if (!h->logged_ppa_sample && in_bytes >= 8u) {
        h->logged_ppa_sample = true;
        ESP_LOGI(TAG, "UYVY PPA 4B swap verified: raw[0..7]=%02x %02x %02x %02x | %02x %02x %02x %02x "
                      "-> ppa[0..7]=%02x %02x %02x %02x | %02x %02x %02x %02x",
                 uyvy_dma_in[0], uyvy_dma_in[1], uyvy_dma_in[2], uyvy_dma_in[3],
                 uyvy_dma_in[4], uyvy_dma_in[5], uyvy_dma_in[6], uyvy_dma_in[7],
                 h->ppa_buf[0], h->ppa_buf[1], h->ppa_buf[2], h->ppa_buf[3],
                 h->ppa_buf[4], h->ppa_buf[5], h->ppa_buf[6], h->ppa_buf[7]);
    }

    h->ppa_valid_bytes = in_bytes;
    h->ppa_valid_w = width;
    h->ppa_valid_h = height;
    return ESP_OK;
}

esp_err_t csi_jpeg_encode_swapped_uyvy(csi_jpeg_handle_t h,
                                       uint32_t width, uint32_t height,
                                       const uint8_t **out_jpeg, size_t *out_len)
{
    if (!h || !h->ppa_buf || !out_jpeg || !out_len) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_jpeg = NULL;
    *out_len = 0;

    if (h->ppa_valid_bytes == 0u || h->ppa_valid_w != width || h->ppa_valid_h != height) {
        return ESP_ERR_INVALID_STATE;
    }

    // Source: ESP-IDF v6.0.1 jpeg_encode.c:213-216, 259-260; docs/P2B_TC358743_REFERENCE.md §6
    // (JPEG_ENCODE_IN_FORMAT_YUV422 with JPEG_DOWN_SAMPLING_YUV422).
    jpeg_encode_cfg_t enc_cfg = {
        .width = width,
        .height = height,
        .src_type = JPEG_ENCODE_IN_FORMAT_YUV422,
        .sub_sample = JPEG_DOWN_SAMPLING_YUV422,
        .image_quality = h->quality,
        .pixel_reverse = false,
    };

    return run_jpeg_encoder(h, &enc_cfg, h->ppa_buf, (uint32_t)h->ppa_valid_bytes, out_jpeg, out_len);
}

size_t csi_jpeg_output_capacity(csi_jpeg_handle_t h)
{
    return h ? h->max_output_bytes : 0u;
}

size_t csi_jpeg_ppa_capacity(csi_jpeg_handle_t h)
{
    return h ? h->ppa_buf_bytes : 0u;
}

esp_err_t csi_jpeg_set_quality(csi_jpeg_handle_t h, uint8_t quality)
{
    if (!h || quality < 1u || quality > 100u) {
        return ESP_ERR_INVALID_ARG;
    }
    h->quality = quality;
    return ESP_OK;
}

uint8_t csi_jpeg_get_quality(csi_jpeg_handle_t h)
{
    return h ? h->quality : 0u;
}

esp_err_t csi_jpeg_close(csi_jpeg_handle_t h)
{
    if (!h) {
        return ESP_ERR_INVALID_ARG;
    }
    if (h->ppa_buf) {
        heap_caps_free(h->ppa_buf);
        h->ppa_buf = NULL;
    }
    if (h->ppa_srm) {
        (void)ppa_unregister_client(h->ppa_srm);
        h->ppa_srm = NULL;
    }
    if (h->out_buf) {
        free(h->out_buf);
        h->out_buf = NULL;
    }
    esp_err_t err = ESP_OK;
    if (h->eng) {
        err = jpeg_del_encoder_engine(h->eng);
        h->eng = NULL;
    }
    free(h);
    return err;
}
