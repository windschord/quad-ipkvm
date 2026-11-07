#include "csi_jpeg.h"

#include <inttypes.h>
#include <stdlib.h>
#include "csi_capture_logic.h"
#include "driver/jpeg_encode.h"
#include "esp_log.h"

static const char *TAG = "csi_jpeg";

#define CSI_JPEG_MIN_BUF_BYTES    4096u
#define CSI_JPEG_DEFAULT_TIMEOUT  100

struct csi_jpeg_t {
    jpeg_encoder_handle_t eng;
    uint8_t *out_buf;
    size_t out_buf_bytes;
    uint8_t quality;
};

esp_err_t csi_jpeg_open(const csi_jpeg_config_t *cfg, csi_jpeg_handle_t *out)
{
    if (!cfg || !out) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = NULL;

    if (cfg->quality < 1u || cfg->quality > 100u ||
        cfg->max_output_bytes < CSI_JPEG_MIN_BUF_BYTES ||
        cfg->max_output_bytes > (size_t)UINT32_MAX) {
        ESP_LOGE(TAG, "Invalid JPEG config: quality=%u max_output_bytes=%u",
                 (unsigned)cfg->quality, (unsigned)cfg->max_output_bytes);
        return ESP_ERR_INVALID_ARG;
    }

    struct csi_jpeg_t *h = calloc(1, sizeof(*h));
    if (!h) {
        return ESP_ERR_NO_MEM;
    }
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
    jpeg_encode_memory_alloc_cfg_t mem_cfg = {
        .buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER,
    };
    size_t alloc_bytes = 0;
    h->out_buf = (uint8_t *)jpeg_alloc_encoder_mem(cfg->max_output_bytes, &mem_cfg, &alloc_bytes);
    if (!h->out_buf || alloc_bytes < cfg->max_output_bytes) {
        ESP_LOGE(TAG, "jpeg_alloc_encoder_mem(%u) failed", (unsigned)cfg->max_output_bytes);
        (void)jpeg_del_encoder_engine(h->eng);
        free(h);
        return ESP_ERR_NO_MEM;
    }
    h->out_buf_bytes = alloc_bytes;

    ESP_LOGI(TAG, "Opened HW JPEG encoder: quality=%u, YUV420, output_buf=%u bytes @ %p",
             (unsigned)h->quality, (unsigned)h->out_buf_bytes, (void *)h->out_buf);
    *out = h;
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

    uint32_t encoded_bytes = 0;
    esp_err_t err = jpeg_encoder_process(h->eng, &enc_cfg,
                                         bgr24_in, (uint32_t)in_bytes,
                                         h->out_buf, (uint32_t)h->out_buf_bytes,
                                         &encoded_bytes);
    if (err != ESP_OK) {
        return err;
    }

    if (encoded_bytes < 4u || (size_t)encoded_bytes > h->out_buf_bytes ||
        h->out_buf[0] != 0xFFu || h->out_buf[1] != 0xD8u ||
        h->out_buf[encoded_bytes - 2u] != 0xFFu || h->out_buf[encoded_bytes - 1u] != 0xD9u) {
        ESP_LOGE(TAG, "Invalid JPEG output: len=%" PRIu32 " head=%02x%02x tail=%02x%02x",
                 encoded_bytes,
                 encoded_bytes >= 1u ? h->out_buf[0] : 0u,
                 encoded_bytes >= 2u ? h->out_buf[1] : 0u,
                 encoded_bytes >= 2u ? h->out_buf[encoded_bytes - 2u] : 0u,
                 encoded_bytes >= 1u ? h->out_buf[encoded_bytes - 1u] : 0u);
        return ESP_ERR_INVALID_RESPONSE;
    }

    *out_jpeg = h->out_buf;
    *out_len = (size_t)encoded_bytes;
    return ESP_OK;
}

size_t csi_jpeg_output_capacity(csi_jpeg_handle_t h)
{
    return h ? h->out_buf_bytes : 0u;
}

esp_err_t csi_jpeg_close(csi_jpeg_handle_t h)
{
    if (!h) {
        return ESP_ERR_INVALID_ARG;
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
