#pragma once
// csi_jpeg: hardware JPEG encoder wrapper for MIPI CSI RGB888 (BGR24 in memory) frames
// using ESP-IDF v6.0.1 esp_driver_jpeg (P2b-5, docs/P2B_PLAN.md).
//
// - ESP32-P4 rev v1.3 CSI DMA stores RGB888 pixels in B, G, R byte order in PSRAM
//   (docs/P2B_TC358743_REFERENCE.md §4-2; verified in docs/HW_VERIFY.md E.1 item 9).
// - ESP-IDF's JPEG_ENCODE_IN_FORMAT_RGB888 is defined as ESP_COLOR_FOURCC_BGR24
//   (esp_driver_jpeg/include/driver/jpeg_types.h:58), so CSI buffers feed the encoder
//   directly with pixel_reverse = false and YUV420 subsampling (docs/P2B_TC358743_REFERENCE.md §6).
// - The compressed output buffer is allocated ONCE via jpeg_alloc_encoder_mem() so both
//   its base address and length are L2-cache-line aligned as required by jpeg_encoder_process()
//   (esp_driver_jpeg/jpeg_encode.c:177, 380-392).

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct csi_jpeg_t *csi_jpeg_handle_t;

typedef struct {
    uint8_t quality;          // 1..100 (reference default: 70)
    size_t max_output_bytes;  // capacity of the reusable PSRAM output buffer (>= 4096)
    int timeout_ms;           // per-frame HW timeout in ms (<= 0 uses default 100 ms)
} csi_jpeg_config_t;

esp_err_t csi_jpeg_open(const csi_jpeg_config_t *cfg, csi_jpeg_handle_t *out);

// Encodes one width x height BGR24 (CSI RGB888) frame into the internal aligned PSRAM buffer.
// On ESP_OK, *out_jpeg points to the internal buffer (valid until the next csi_jpeg_encode()
// or csi_jpeg_close()) and *out_len is the baseline JPEG byte count (SOI FFD8 .. EOI FFD9).
esp_err_t csi_jpeg_encode(csi_jpeg_handle_t h,
                          const uint8_t *bgr24_in, size_t in_bytes,
                          uint32_t width, uint32_t height,
                          const uint8_t **out_jpeg, size_t *out_len);

size_t csi_jpeg_output_capacity(csi_jpeg_handle_t h);

esp_err_t csi_jpeg_close(csi_jpeg_handle_t h);

#ifdef __cplusplus
}
#endif
