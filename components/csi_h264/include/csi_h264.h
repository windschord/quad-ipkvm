#pragma once
// csi_h264: ESP32-P4 rev v1.3 hardware H.264 Baseline encoder wrapper (P7, docs/VIDEO_TRANSPORT.md §9).
//
// Pipeline on rev v1.3:
// 1. PPA SRM converts CSI RGB888 (BGR24 byte order) to O_UYY_E_VYY (PPA_SRM_COLOR_MODE_YUV420).
//    For non-16-aligned dimensions such as 1920x1080, the PSRAM YUV buffer is sized for 16-px aligned
//    macroblocks (1920x1088 = 3,133,440 B) and zero-padded after the active lines (3,110,400 B).
// 2. espressif/esp_h264 hardware encoder outputs Annex B NALUs (IDR frames include SPS+PPS).
// 3. Two allocation modes are supported for P7 memory verification (TODO(hw-verify)):
//    - prealloc_buffers=true: allocate PPA SRM client + max-mode PSRAM YUV/output buffers at create().
//    - prealloc_buffers=false: allocate PSRAM buffers + encoder on demand when H.264 is active, and
//      free them all in csi_h264_release_stream() on switch back to MJPEG or mode change.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Returned when H.264 encoded output overflows out_buf or exceeds max_output_bytes.
#define CSI_H264_ERR_OUTPUT_OVERSIZE ((esp_err_t)0x6201)

typedef struct csi_h264_t *csi_h264_handle_t;

typedef struct {
    uint32_t max_width;           // PSRAM YUV buffer max width (e.g. 1920)
    uint32_t max_height;          // PSRAM YUV buffer max height (e.g. 1080)
    size_t max_output_bytes;      // Frame Hub max_frame_size bound (e.g. 768 * 1024)
    uint8_t gop;                  // IDR cadence (1..255 frames)
    uint32_t bitrate_720p_bps;    // target bitrate for <= 1280x720
    uint32_t bitrate_1080p_bps;   // target bitrate for > 720p up to 1920x1080
    uint8_t qp_min;               // minimum QP (0..51)
    uint8_t qp_max;               // maximum QP (qp_min..51)
    bool prealloc_buffers;        // true = preallocate PPA + max PSRAM buffers at create()
} csi_h264_config_t;

typedef struct {
    const uint8_t *data;
    size_t len;
    bool is_key;
    uint32_t ppa_us;
    uint32_t enc_us;
} csi_h264_frame_out_t;

esp_err_t csi_h264_create(const csi_h264_config_t *cfg, csi_h264_handle_t *out);

// Step 1: Converts one BGR24 (CSI RGB888) frame into the internal O_UYY_E_VYY PSRAM buffer via PPA SRM.
// Allocates buffers on demand when prealloc_buffers == false. Caller can immediately release the CSI
// capture buffer once this function returns.
esp_err_t csi_h264_ppa_rgb_to_yuv(csi_h264_handle_t h,
                                  const uint8_t *bgr24_in, size_t in_bytes,
                                  uint32_t width, uint32_t height,
                                  uint32_t *out_ppa_us);

// Step 2: Encodes the prepared O_UYY_E_VYY buffer into Annex B H.264 for (width x height).
// (Re)opens esp_h264 HW encoder when not active or when resolution changed.
// On ESP_OK, out->data points to internal PSRAM buffer valid until next encode/release/destroy.
// Returns CSI_H264_ERR_OUTPUT_OVERSIZE if bitstream exceeds out buffer or max_output_bytes.
esp_err_t csi_h264_encode_prepared(csi_h264_handle_t h,
                                   uint32_t width, uint32_t height,
                                   bool force_idr, uint32_t pts_ms,
                                   csi_h264_frame_out_t *out);

// Closes the HW encoder handle (releasing internal DMA/ref RAM) and, when prealloc_buffers == false,
// also unregisters PPA and frees PSRAM YUV/output buffers. Safe to call repeatedly.
void csi_h264_release_stream(csi_h264_handle_t h);

size_t csi_h264_yuv_capacity(csi_h264_handle_t h);
size_t csi_h264_output_capacity(csi_h264_handle_t h);
bool csi_h264_is_stream_active(csi_h264_handle_t h);

esp_err_t csi_h264_destroy(csi_h264_handle_t h);

#ifdef __cplusplus
}
#endif
