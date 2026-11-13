#pragma once
// csi_video_source: video_source_t implementation that captures HDMI frames via MIPI CSI
// (TC358743 -> csi_capture), encodes baseline JPEG via esp_driver_jpeg (csi_jpeg),
// and publishes frames to frame_hub (P2b-6, docs/INTERFACES.md §1.1-§1.3).

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "csi_video_source_logic.h"
#include "tc358743.h"
#include "video_source.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t max_width;           // PSRAM capture buffer max width (e.g. 1920)
    uint32_t max_height;          // PSRAM capture buffer max height (e.g. 1080)
    int lane_bit_rate_mbps;       // CSI lane rate matching TC358743 PLL (972)
    uint8_t data_lanes;           // CSI data lanes (2 on ESP32-P4-ETH)
    bool create_isp_bypass;       // create ISP bypass processor (reference configuration)
    uint8_t jpeg_quality;         // HW JPEG quality 1..100 (default 70)
    size_t hub_max_frame_bytes;   // Frame Hub max_frame_size for §1.2(b) oversize accounting
    tc358743_format_t pixel_format; // TC358743_FORMAT_RGB888 (0, default) or TC358743_FORMAT_UYVY
} csi_video_source_cfg_t;

// Binds the initialized TC358743 handle and capture/encode parameters before start() is called.
void csi_video_source_configure(tc358743_handle_t tc_dev, const csi_video_source_cfg_t *cfg);

// Returns the singleton video_source_t ("csi") whose start() spawns the publisher task
// without blocking and whose stop() requests shutdown and waits up to 2.5 s for the task
// to halt CSI DMA and free capture/JPEG resources (docs/INTERFACES.md §1.1).
// Note: if stop() hits the 2.5 s wait bound, teardown may still be completing in the background;
// callers must NOT destroy frame_hub before the task finishes.
const video_source_t *csi_video_source_get(void);

// Non-blocking status accessors safe to call from any task or core without locks
// (docs/INTERFACES.md §1.3).
kvm_video_sync_state_t csi_video_source_get_sync_state(void);
void csi_video_source_get_mode(uint32_t *width, uint32_t *height);
void csi_video_source_get_status(csi_video_status_t *out);

#ifdef __cplusplus
}
#endif
