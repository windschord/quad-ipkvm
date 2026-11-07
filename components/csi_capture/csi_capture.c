#include "csi_capture.h"

#include <stdlib.h>
#include <string.h>
#include "esp_attr.h"
#include "esp_cache.h"
#include "esp_cam_ctlr.h"
#include "esp_cam_ctlr_csi.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/isp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "soc/mipi_csi_bridge_struct.h"
#include "soc/mipi_csi_host_struct.h"

static const char *TAG = "csi_capture";

// RGB888 on the wire (CSI-2 data type 0x24) and in memory (bridge colour conversion bypassed:
// input == output). Memory order is B,G,R per pixel on rev v1.3 [参照].
// Source: docs/P2B_TC358743_REFERENCE.md §4-1, §4-2
#define CSI_CAPTURE_BPP 24

// DMA works in 64-bit beats with 512-byte bursts; 128-byte alignment covers both 64B and 128B
// ESP32-P4 L2 cache line configurations for esp_cache_msync.
// Source: ESP-IDF v6.0.1 esp_cam_ctlr_csi.c (burst_len 512, 64-bit width), esp_cache.h
#define CSI_CAPTURE_BUF_ALIGN 128

// ISP clock for the bypass processor. Source: docs/P2B_TC358743_REFERENCE.md §4-1 (80 MHz)
#define CSI_CAPTURE_ISP_CLK_HZ 80000000

struct csi_capture_t {
    esp_cam_ctlr_handle_t ctlr;
    isp_proc_handle_t isp;
    uint8_t *buf[CSI_PP_NUM_BUFS];
    size_t buf_bytes;
    size_t frame_bytes;
    uint32_t width;
    uint32_t height;
    uint32_t max_width;
    uint32_t max_height;
    int lane_bit_rate_mbps;
    uint8_t data_lanes;
    uint32_t skip_frames;
    bool create_isp_bypass;
    bool enabled;
    bool started;
    csi_pp_t pp;
    portMUX_TYPE lock;
    SemaphoreHandle_t frame_sem;
    int64_t first_frame_us;
    int64_t last_frame_us;
};

// Called by the driver at start (task context) and from the DMA ISR after every completion,
// BEFORE on_trans_finished. Picks the next destination buffer.
static bool IRAM_ATTR on_get_new_trans(esp_cam_ctlr_handle_t handle, esp_cam_ctlr_trans_t *trans, void *user_data) {
    (void)handle;
    struct csi_capture_t *h = (struct csi_capture_t *)user_data;
    portENTER_CRITICAL_SAFE(&h->lock);
    int idx = csi_pp_next_dma(&h->pp);
    portEXIT_CRITICAL_SAFE(&h->lock);
    trans->buffer = h->buf[idx];
    trans->buflen = h->buf_bytes;
    return false;
}

static bool IRAM_ATTR on_trans_finished(esp_cam_ctlr_handle_t handle, esp_cam_ctlr_trans_t *trans, void *user_data) {
    (void)handle;
    (void)trans;
    struct csi_capture_t *h = (struct csi_capture_t *)user_data;
    int64_t now = esp_timer_get_time();
    if (h->first_frame_us == 0) h->first_frame_us = now;
    h->last_frame_us = now;
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(h->frame_sem, &woken);
    return woken == pdTRUE;
}

static void destroy_ctlr_and_isp(struct csi_capture_t *h) {
    if (!h) return;
    if (h->isp) {
        esp_isp_del_processor(h->isp);
        h->isp = NULL;
    }
    if (h->enabled && h->ctlr) {
        esp_cam_ctlr_disable(h->ctlr);
        h->enabled = false;
    }
    if (h->ctlr) {
        esp_cam_ctlr_del(h->ctlr);
        h->ctlr = NULL;
    }
}

static void free_handle(struct csi_capture_t *h) {
    if (!h) return;
    destroy_ctlr_and_isp(h);
    for (int i = 0; i < CSI_PP_NUM_BUFS; i++) heap_caps_free(h->buf[i]);
    if (h->frame_sem) vSemaphoreDelete(h->frame_sem);
    free(h);
}

static esp_err_t create_ctlr_and_isp(struct csi_capture_t *h, uint32_t width, uint32_t height, size_t frame_bytes) {
    esp_err_t ret = ESP_OK;
    esp_cam_ctlr_csi_config_t csi_cfg = {
        .ctlr_id = 0,
        .clk_src = MIPI_CSI_PHY_CLK_SRC_DEFAULT,
        .h_res = width,
        .v_res = height,
        .data_lane_num = h->data_lanes,
        .lane_bit_rate_mbps = h->lane_bit_rate_mbps,
        .input_data_color_type = CAM_CTLR_COLOR_RGB888,
        .output_data_color_type = CAM_CTLR_COLOR_RGB888,   // same as input -> colour conversion bypassed
        .queue_items = 1,
        .byte_swap_en = false,
        .bk_buffer_dis = true,
    };
    ESP_GOTO_ON_ERROR(esp_cam_new_csi_ctlr(&csi_cfg, &h->ctlr), err, TAG, "esp_cam_new_csi_ctlr failed");

    esp_cam_ctlr_evt_cbs_t cbs = {
        .on_get_new_trans = on_get_new_trans,
        .on_trans_finished = on_trans_finished,
    };
    ESP_GOTO_ON_ERROR(esp_cam_ctlr_register_event_callbacks(h->ctlr, &cbs, h), err, TAG, "register callbacks failed");
    ESP_GOTO_ON_ERROR(esp_cam_ctlr_enable(h->ctlr), err, TAG, "esp_cam_ctlr_enable failed");
    h->enabled = true;

    if (h->create_isp_bypass) {
        // Source: docs/P2B_TC358743_REFERENCE.md §4-1 (bypass_isp, CSI source, 80 MHz, no line packets).
        // Whether this processor is required at all is unverified (TODO(hw-verify)).
        esp_isp_processor_cfg_t isp_cfg = {
            .clk_hz = CSI_CAPTURE_ISP_CLK_HZ,
            .input_data_source = ISP_INPUT_DATA_SOURCE_CSI,
            .input_data_color_type = ISP_COLOR_RGB888,
            .output_data_color_type = ISP_COLOR_RGB888,
            .has_line_start_packet = false,
            .has_line_end_packet = false,
            .h_res = width,
            .v_res = height,
            .flags.bypass_isp = true,
        };
        ESP_GOTO_ON_ERROR(esp_isp_new_processor(&isp_cfg, &h->isp), err, TAG, "esp_isp_new_processor (bypass) failed");
    }

    h->width = width;
    h->height = height;
    h->frame_bytes = frame_bytes;
    return ESP_OK;

err:
    destroy_ctlr_and_isp(h);
    return ret;
}

esp_err_t csi_capture_open_pool(const csi_capture_config_t *cfg, csi_capture_handle_t *out) {
    ESP_RETURN_ON_FALSE(cfg && out, ESP_ERR_INVALID_ARG, TAG, "null argument");
    ESP_RETURN_ON_FALSE(cfg->data_lanes >= 1 && cfg->data_lanes <= 2, ESP_ERR_INVALID_ARG, TAG, "lanes must be 1..2");
    ESP_RETURN_ON_FALSE(cfg->lane_bit_rate_mbps > 0, ESP_ERR_INVALID_ARG, TAG, "lane rate");
    *out = NULL;

    size_t max_bytes = 0;
    csi_geom_result_t g = csi_capture_check_geometry(cfg->max_width, cfg->max_height, CSI_CAPTURE_BPP,
                                                     cfg->max_width, cfg->max_height, SIZE_MAX, &max_bytes);
    ESP_RETURN_ON_FALSE(g == CSI_GEOM_OK, ESP_ERR_INVALID_ARG, TAG, "invalid max geometry %"PRIu32"x%"PRIu32" (%d)",
                        cfg->max_width, cfg->max_height, (int)g);
    size_t buf_bytes = csi_capture_round_up(max_bytes, CSI_CAPTURE_BUF_ALIGN);
    ESP_RETURN_ON_FALSE(buf_bytes != 0, ESP_ERR_INVALID_ARG, TAG, "buffer size overflow");

    struct csi_capture_t *h = calloc(1, sizeof(*h));
    ESP_RETURN_ON_FALSE(h, ESP_ERR_NO_MEM, TAG, "no mem");
    h->lock = (portMUX_TYPE)portMUX_INITIALIZER_UNLOCKED;
    h->max_width = cfg->max_width;
    h->max_height = cfg->max_height;
    h->lane_bit_rate_mbps = cfg->lane_bit_rate_mbps;
    h->data_lanes = cfg->data_lanes;
    h->skip_frames = cfg->skip_frames;
    h->create_isp_bypass = cfg->create_isp_bypass;
    h->buf_bytes = buf_bytes;
    csi_pp_init(&h->pp, cfg->skip_frames);

    esp_err_t ret = ESP_OK;
    h->frame_sem = xSemaphoreCreateBinary();
    ESP_GOTO_ON_FALSE(h->frame_sem, ESP_ERR_NO_MEM, err, TAG, "no mem for semaphore");

    for (int i = 0; i < CSI_PP_NUM_BUFS; i++) {
        h->buf[i] = heap_caps_aligned_calloc(CSI_CAPTURE_BUF_ALIGN, 1, buf_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
        ESP_GOTO_ON_FALSE(h->buf[i], ESP_ERR_NO_MEM, err, TAG, "no PSRAM for buffer %d (%u bytes)", i, (unsigned)buf_bytes);
        esp_cache_msync(h->buf[i], buf_bytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_INVALIDATE);
    }

    *out = h;
    return ESP_OK;

err:
    free_handle(h);
    return ret;
}

esp_err_t csi_capture_reconfigure(csi_capture_handle_t h, uint32_t width, uint32_t height) {
    ESP_RETURN_ON_FALSE(h, ESP_ERR_INVALID_ARG, TAG, "null handle");

    // Always halt ongoing DMA and tear down previous controller BEFORE inspecting or configuring
    // a new geometry so stale transfers can never continue on mode changes.
    ESP_RETURN_ON_ERROR(csi_capture_stop(h), TAG, "stop before reconfigure failed");
    destroy_ctlr_and_isp(h);
    h->width = 0;
    h->height = 0;
    h->frame_bytes = 0;

    size_t frame_bytes = 0;
    csi_geom_result_t g = csi_capture_check_geometry(width, height, CSI_CAPTURE_BPP,
                                                     h->max_width, h->max_height, h->buf_bytes, &frame_bytes);
    if (g != CSI_GEOM_OK) {
        ESP_LOGE(TAG, "Rejecting frame %"PRIu32"x%"PRIu32" before DMA setup (result %d, capacity %u)",
                 width, height, (int)g, (unsigned)h->buf_bytes);
        return ESP_ERR_INVALID_SIZE;
    }

    portENTER_CRITICAL(&h->lock);
    csi_pp_init(&h->pp, h->skip_frames);
    h->first_frame_us = 0;
    h->last_frame_us = 0;
    portEXIT_CRITICAL(&h->lock);
    xSemaphoreTake(h->frame_sem, 0);

    for (int i = 0; i < CSI_PP_NUM_BUFS; i++) {
        esp_cache_msync(h->buf[i], h->buf_bytes, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
    }

    ESP_RETURN_ON_ERROR(create_ctlr_and_isp(h, width, height, frame_bytes), TAG, "recreate ctlr/isp failed");
    ESP_LOGI(TAG, "Configured: %"PRIu32"x%"PRIu32" RGB888, %u lanes @ %d Mbps, frame %u bytes, 2 x %u-byte PSRAM buffers, ISP bypass %s",
             width, height, (unsigned)h->data_lanes, h->lane_bit_rate_mbps,
             (unsigned)frame_bytes, (unsigned)h->buf_bytes, h->isp ? "created" : "not created");
    return ESP_OK;
}

esp_err_t csi_capture_open(const csi_capture_config_t *cfg, csi_capture_handle_t *out) {
    ESP_RETURN_ON_FALSE(cfg && out, ESP_ERR_INVALID_ARG, TAG, "null argument");
    ESP_RETURN_ON_FALSE(cfg->data_lanes >= 1 && cfg->data_lanes <= 2, ESP_ERR_INVALID_ARG, TAG, "lanes must be 1..2");
    ESP_RETURN_ON_FALSE(cfg->lane_bit_rate_mbps > 0, ESP_ERR_INVALID_ARG, TAG, "lane rate");
    *out = NULL;

    size_t max_bytes = 0;
    csi_geom_result_t g = csi_capture_check_geometry(cfg->max_width, cfg->max_height, CSI_CAPTURE_BPP,
                                                     cfg->max_width, cfg->max_height, SIZE_MAX, &max_bytes);
    ESP_RETURN_ON_FALSE(g == CSI_GEOM_OK, ESP_ERR_INVALID_ARG, TAG, "invalid max geometry %"PRIu32"x%"PRIu32" (%d)",
                        cfg->max_width, cfg->max_height, (int)g);
    size_t buf_bytes = csi_capture_round_up(max_bytes, CSI_CAPTURE_BUF_ALIGN);
    ESP_RETURN_ON_FALSE(buf_bytes != 0, ESP_ERR_INVALID_ARG, TAG, "buffer size overflow");

    size_t frame_bytes = 0;
    g = csi_capture_check_geometry(cfg->width, cfg->height, CSI_CAPTURE_BPP,
                                   cfg->max_width, cfg->max_height, buf_bytes, &frame_bytes);
    if (g != CSI_GEOM_OK) {
        ESP_LOGE(TAG, "Rejecting frame %"PRIu32"x%"PRIu32" before DMA setup (result %d, capacity %u)",
                 cfg->width, cfg->height, (int)g, (unsigned)buf_bytes);
        return ESP_ERR_INVALID_SIZE;
    }

    csi_capture_handle_t h = NULL;
    ESP_RETURN_ON_ERROR(csi_capture_open_pool(cfg, &h), TAG, "open_pool failed");
    esp_err_t ret = csi_capture_reconfigure(h, cfg->width, cfg->height);
    if (ret != ESP_OK) {
        free_handle(h);
        return ret;
    }
    *out = h;
    return ESP_OK;
}

esp_err_t csi_capture_start(csi_capture_handle_t h) {
    ESP_RETURN_ON_FALSE(h && h->enabled, ESP_ERR_INVALID_STATE, TAG, "not open");
    ESP_RETURN_ON_FALSE(!h->started, ESP_ERR_INVALID_STATE, TAG, "already started");
    xSemaphoreTake(h->frame_sem, 0);
    ESP_RETURN_ON_ERROR(esp_cam_ctlr_start(h->ctlr), TAG, "esp_cam_ctlr_start failed");
    h->started = true;
    return ESP_OK;
}

esp_err_t csi_capture_acquire(csi_capture_handle_t h, uint32_t timeout_ms,
                              const uint8_t **out_data, size_t *out_len, int *out_idx) {
    ESP_RETURN_ON_FALSE(h && out_data && out_len && out_idx, ESP_ERR_INVALID_ARG, TAG, "null argument");
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    for (;;) {
        portENTER_CRITICAL(&h->lock);
        int idx = csi_pp_acquire(&h->pp);
        portEXIT_CRITICAL(&h->lock);
        if (idx != CSI_PP_NONE) {
            // Drop any cache lines covering the buffer so the CPU reads what the DMA wrote.
            esp_cache_msync(h->buf[idx], h->buf_bytes, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
            *out_data = h->buf[idx];
            *out_len = h->frame_bytes;
            *out_idx = idx;
            return ESP_OK;
        }
        TickType_t now = xTaskGetTickCount();
        if ((int32_t)(deadline - now) <= 0) return ESP_ERR_TIMEOUT;
        xSemaphoreTake(h->frame_sem, deadline - now);
    }
}

esp_err_t csi_capture_release(csi_capture_handle_t h, int idx) {
    ESP_RETURN_ON_FALSE(h, ESP_ERR_INVALID_ARG, TAG, "null handle");
    portENTER_CRITICAL(&h->lock);
    bool ok = csi_pp_release(&h->pp, idx);
    portEXIT_CRITICAL(&h->lock);
    return ok ? ESP_OK : ESP_ERR_INVALID_ARG;
}

esp_err_t csi_capture_stop(csi_capture_handle_t h) {
    ESP_RETURN_ON_FALSE(h, ESP_ERR_INVALID_ARG, TAG, "null handle");
    if (!h->started) return ESP_OK;
    ESP_RETURN_ON_ERROR(esp_cam_ctlr_stop(h->ctlr), TAG, "esp_cam_ctlr_stop failed");
    portENTER_CRITICAL(&h->lock);
    csi_pp_stop(&h->pp);
    portEXIT_CRITICAL(&h->lock);
    h->started = false;
    return ESP_OK;
}

void csi_capture_get_stats(csi_capture_handle_t h, csi_capture_stats_t *out) {
    if (!h || !out) return;
    portENTER_CRITICAL(&h->lock);
    out->completed = h->pp.completed;
    out->published = h->pp.published;
    out->overwritten = h->pp.overwritten;
    out->first_frame_us = h->first_frame_us;
    out->last_frame_us = h->last_frame_us;
    portEXIT_CRITICAL(&h->lock);
    out->width = h->width;
    out->height = h->height;
    out->frame_bytes = h->frame_bytes;
    out->buffer_bytes = h->buf_bytes;
}

void csi_capture_log_hw_state(csi_capture_handle_t h, const char *label) {
    if (!h || !h->enabled) return;
    csi_host_phy_rx_reg_t phy_rx = { .val = MIPI_CSI_HOST.phy_rx.val };
    csi_host_phy_stopstate_reg_t stopst = { .val = MIPI_CSI_HOST.phy_stopstate.val };
    csi_brg_frame_cfg_reg_t fcfg = { .val = MIPI_CSI_BRIDGE.frame_cfg.val };
    csi_brg_data_type_cfg_reg_t dtcfg = { .val = MIPI_CSI_BRIDGE.data_type_cfg.val };
    uint32_t brg_en = MIPI_CSI_BRIDGE.csi_en.val;
    uint32_t int_raw = MIPI_CSI_BRIDGE.int_raw.val;
    ESP_LOGI(TAG, "HW[%s]: phy_rx=0x%08"PRIx32" (clk_hs=%u ulps_clknot=%u) stopstate=0x%08"PRIx32
             " brg_en=%"PRIu32" frame_cfg=0x%08"PRIx32" (h=%u v=%u hsync=%u vchk=%u)"
             " dt=0x%02x..0x%02x int_raw=0x%02"PRIx32,
             label ? label : "", phy_rx.val, (unsigned)phy_rx.phy_rxclkactivehs, (unsigned)phy_rx.phy_rxulpsclknot,
             stopst.val, brg_en, fcfg.val, (unsigned)fcfg.hadr_num, (unsigned)fcfg.vadr_num,
             (unsigned)fcfg.has_hsync_e, (unsigned)fcfg.vadr_num_check,
             (unsigned)dtcfg.data_type_min, (unsigned)dtcfg.data_type_max, int_raw);
}

esp_err_t csi_capture_close(csi_capture_handle_t h) {
    ESP_RETURN_ON_FALSE(h, ESP_ERR_INVALID_ARG, TAG, "null handle");
    ESP_RETURN_ON_ERROR(csi_capture_stop(h), TAG, "stop failed");
    free_handle(h);
    return ESP_OK;
}
