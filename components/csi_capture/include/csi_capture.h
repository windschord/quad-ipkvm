#pragma once
// csi_capture: receives RGB888 frames from the TC358743 over MIPI CSI-2 into PSRAM
// using the ESP-IDF v6.0.1 esp_cam_ctlr_csi driver (P2b-4, docs/P2B_PLAN.md).
//
// - Two frame buffers sized for the maximum mode are allocated ONCE at open (never re-allocated),
//   because the driver's backup buffer re-allocation fragments PSRAM.
//   Source: docs/P2B_TC358743_REFERENCE.md §4-1 (bk_buffer_dis = true).
// - The geometry is validated against the buffer capacity BEFORE the DMA is configured.
//   Source: docs/P2B_TC358743_REFERENCE.md §7 item 14.
// - Call csi_capture_stop() before esp_restart(): the CSI DMA writes PSRAM almost continuously.
//   Source: docs/P2B_TC358743_REFERENCE.md §7 item 13.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "csi_capture_logic.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct csi_capture_t *csi_capture_handle_t;

typedef struct {
    uint32_t width;               // active pixels per line (e.g. from TC358743 DE_WIDTH_H)
    uint32_t height;              // active lines per frame (e.g. from TC358743 DE_WIDTH_V)
    uint32_t max_width;           // buffer sizing (maximum mode)
    uint32_t max_height;
    int lane_bit_rate_mbps;       // must match the TC358743 PLL (972)
    uint8_t data_lanes;           // 2 on ESP32-P4-ETH (J1 has no lanes 2/3)
    uint32_t skip_frames;         // initial completions to discard (the first may be partial)
    bool create_isp_bypass;       // create an ISP processor with bypass_isp = true (reference config)
} csi_capture_config_t;

typedef struct {
    uint32_t width;
    uint32_t height;
    size_t frame_bytes;           // width * height * 3
    size_t buffer_bytes;          // capacity of each buffer
    uint32_t completed;           // DMA completions
    uint32_t published;           // completions offered to the consumer
    uint32_t overwritten;         // frames overwritten before being consumed
    int64_t last_frame_us;        // esp_timer time of the last completion (0 if none)
    int64_t first_frame_us;       // esp_timer time of the first completion (0 if none)
} csi_capture_stats_t;

esp_err_t csi_capture_open(const csi_capture_config_t *cfg, csi_capture_handle_t *out);
esp_err_t csi_capture_start(csi_capture_handle_t h);

// Waits up to timeout_ms for a new complete frame and hands it to the caller.
// The buffer stays valid (the DMA will not write it) until csi_capture_release().
esp_err_t csi_capture_acquire(csi_capture_handle_t h, uint32_t timeout_ms,
                              const uint8_t **out_data, size_t *out_len, int *out_idx);
esp_err_t csi_capture_release(csi_capture_handle_t h, int idx);

esp_err_t csi_capture_stop(csi_capture_handle_t h);
void csi_capture_get_stats(csi_capture_handle_t h, csi_capture_stats_t *out);
void csi_capture_log_hw_state(csi_capture_handle_t h, const char *label);
esp_err_t csi_capture_close(csi_capture_handle_t h);

#ifdef __cplusplus
}
#endif
