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

#include <stdbool.h>
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
    bool enable_uyvy_ppa;     // P2b-8: allocate PPA SRM client + 16bpp PSRAM swap buffer for UYVY
    uint32_t max_width;       // required when enable_uyvy_ppa == true
    uint32_t max_height;      // required when enable_uyvy_ppa == true
} csi_jpeg_config_t;

esp_err_t csi_jpeg_open(const csi_jpeg_config_t *cfg, csi_jpeg_handle_t *out);

// Encodes one width x height BGR24 (CSI RGB888) frame into the internal aligned PSRAM buffer.
// On ESP_OK, *out_jpeg points to the internal buffer (valid until the next csi_jpeg_encode()
// or csi_jpeg_close()) and *out_len is the baseline JPEG byte count (SOI FFD8 .. EOI FFD9).
esp_err_t csi_jpeg_encode(csi_jpeg_handle_t h,
                          const uint8_t *bgr24_in, size_t in_bytes,
                          uint32_t width, uint32_t height,
                          const uint8_t **out_jpeg, size_t *out_len);

// P2b-8 step 1: Reverses each 32-bit word of a CSI UYVY frame (Y1,V0,Y0,U0 -> U0,Y0,V0,Y1)
// into the internal PPA aligned PSRAM buffer by running PPA SRM in ARGB8888 mode (width/2 x height)
// with rgb_swap = true. The caller may immediately release the CSI ping-pong buffer once this returns.
// Source: ESP-IDF v6.0.1 driver/ppa.h, hal/ppa_ll.h:438-446; docs/P2B_TC358743_REFERENCE.md §6, §10 #9.
esp_err_t csi_jpeg_ppa_swap_uyvy(csi_jpeg_handle_t h,
                                 const uint8_t *uyvy_dma_in, size_t in_bytes,
                                 uint32_t width, uint32_t height);

// P2b-8 step 2: Encodes the most recently PPA-swapped width x height UYVY frame into baseline JPEG
// (src_type = JPEG_ENCODE_IN_FORMAT_YUV422, sub_sample = JPEG_DOWN_SAMPLING_YUV422).
esp_err_t csi_jpeg_encode_swapped_uyvy(csi_jpeg_handle_t h,
                                       uint32_t width, uint32_t height,
                                       const uint8_t **out_jpeg, size_t *out_len);

size_t csi_jpeg_output_capacity(csi_jpeg_handle_t h);
size_t csi_jpeg_ppa_capacity(csi_jpeg_handle_t h);

esp_err_t csi_jpeg_close(csi_jpeg_handle_t h);

#ifdef __cplusplus
}
#endif
