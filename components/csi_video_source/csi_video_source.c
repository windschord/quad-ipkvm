#include "csi_video_source.h"

#include <inttypes.h>
#include <string.h>
#include "csi_capture.h"
#include "csi_jpeg.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "csi_video_src";

#define CSI_VIDEO_TASK_STACK       6144u
#define CSI_VIDEO_TASK_PRIO        5
#define CSI_VIDEO_ACQUIRE_WAIT_MS  200u
#define CSI_VIDEO_POLL_SLICE_MS    50u
#define CSI_VIDEO_POLL_SLICES      5u   // 250 ms between I2C active-size polls when unlocked
#define CSI_VIDEO_STATS_PERIOD_US  (5 * 1000 * 1000)
#define CSI_VIDEO_STOP_WAIT_MS     2500u

static tc358743_handle_t s_tc_dev = NULL;
static csi_video_source_cfg_t s_cfg;
static bool s_configured = false;

static frame_hub_t *s_hub = NULL;
static TaskHandle_t s_task_handle = NULL;
static SemaphoreHandle_t s_done_sem = NULL;
static int s_stop_requested = 0;
static bool s_shutdown_registered = false;

// Active CSI capture handle exposed atomically for the esp_restart() shutdown hook.
// Source: ESP-IDF v6.0.1 esp_system.h:76 (peripherals are not reset across esp_restart())
// and docs/P2B_TC358743_REFERENCE.md §7 item 13 (ongoing CSI DMA across warm restart hangs boot).
static csi_capture_handle_t s_active_cap = NULL;

// Lock-free seqlock status store (docs/INTERFACES.md §1.3).
static csi_video_seq_t s_seq_status;

static void csi_video_shutdown_handler(void)
{
    csi_capture_handle_t cap = __atomic_load_n(&s_active_cap, __ATOMIC_SEQ_CST);
    if (cap) {
        (void)csi_capture_stop(cap);
    }
}

static bool stop_requested(void)
{
    return __atomic_load_n(&s_stop_requested, __ATOMIC_SEQ_CST) != 0;
}

static bool sleep_interruptible_ms(uint32_t total_ms)
{
    uint32_t waited = 0u;
    while (waited < total_ms) {
        if (stop_requested()) {
            return false;
        }
        uint32_t step = total_ms - waited;
        if (step > CSI_VIDEO_POLL_SLICE_MS) {
            step = CSI_VIDEO_POLL_SLICE_MS;
        }
        vTaskDelay(pdMS_TO_TICKS(step));
        waited += step;
    }
    return !stop_requested();
}

static void log_periodic_stats(const csi_video_pub_stats_t *pub,
                               csi_capture_handle_t cap,
                               uint32_t width, uint32_t height,
                               int64_t elapsed_us)
{
    csi_capture_stats_t cap_st;
    csi_capture_get_stats(cap, &cap_st);

    frame_hub_stats_t hub_st = {0};
    if (s_hub) {
        frame_hub_get_stats(s_hub, &hub_st);
    }

    double secs = elapsed_us > 0 ? ((double)elapsed_us / 1e6) : 0.0;
    double enc_fps = (secs > 0.0) ? ((double)pub->win_frames / secs) : 0.0;
    double avg_enc_ms = (pub->win_frames > 0u)
                            ? ((double)pub->win_encode_us / (double)pub->win_frames / 1000.0)
                            : 0.0;
    double max_enc_ms = (double)pub->win_max_encode_us / 1000.0;
    size_t avg_bytes = (pub->win_frames > 0u)
                           ? (size_t)(pub->win_jpeg_bytes / pub->win_frames)
                           : 0u;

    ESP_LOGI(TAG,
             "%" PRIu32 "x%" PRIu32 " @ %.2f fps | enc avg=%.2f ms max=%.2f ms | "
             "jpeg min=%u avg=%u max=%u (peak=%u) B | "
             "pub=%" PRIu64 " busy_drop=%" PRIu64 " oversize_drop=%" PRIu64 " enc_err=%" PRIu64 " | "
             "hub clients=%d dropped=%" PRIu64 " | csi done=%" PRIu32 " ovw=%" PRIu32 " | stack_free=%u B",
             width, height, enc_fps, avg_enc_ms, max_enc_ms,
             (unsigned)pub->win_min_jpeg_bytes, (unsigned)avg_bytes,
             (unsigned)pub->win_max_jpeg_bytes, (unsigned)pub->peak_jpeg_bytes,
             pub->published, pub->pool_busy_drops, pub->oversize_drops, pub->encode_errors,
             hub_st.active_clients, hub_st.published_frames ? hub_st.dropped_frames : hub_st.dropped_frames,
             cap_st.completed, cap_st.overwritten,
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
}

static void csi_video_task(void *arg)
{
    (void)arg;
    csi_capture_handle_t cap = NULL;
    csi_jpeg_handle_t jpeg = NULL;
    uint32_t active_w = 0u, active_h = 0u;

    // 1. Poll TC358743 until HDMI SYNC is locked and geometry fits max buffer bounds.
    uint32_t wait_logs = 0u;
    while (!stop_requested()) {
        uint32_t w = 0u, h = 0u;
        esp_err_t err = tc358743_get_active_size(s_tc_dev, &w, &h);
        if (err == ESP_OK) {
            size_t max_cap = 0u;
            size_t raw_bytes = 0u;
            csi_geom_result_t cap_ok = csi_capture_check_geometry(
                s_cfg.max_width, s_cfg.max_height, 24u,
                s_cfg.max_width, s_cfg.max_height, SIZE_MAX, &max_cap);
            csi_geom_result_t gerr = (cap_ok == CSI_GEOM_OK)
                ? csi_capture_check_geometry(w, h, 24u, s_cfg.max_width, s_cfg.max_height, max_cap, &raw_bytes)
                : cap_ok;
            if (gerr == CSI_GEOM_OK) {
                active_w = w;
                active_h = h;
                break;
            }
            if ((wait_logs++ % 20u) == 0u) {
                ESP_LOGW(TAG, "Rejected out-of-range HDMI mode %" PRIu32 "x%" PRIu32
                              " (max %" PRIu32 "x%" PRIu32 ", geom_err=%d)",
                         w, h, s_cfg.max_width, s_cfg.max_height, (int)gerr);
            }
        }
        csi_video_status_t cur;
        csi_video_seq_read(&s_seq_status, CSI_VIDEO_SEQ_DEFAULT_RETRIES, &cur);
        if (cur.state != KVM_VIDEO_SYNC_NO_SIGNAL) {
            csi_video_seq_write(&s_seq_status, KVM_VIDEO_SYNC_NO_SIGNAL, 0u, 0u);
            ESP_LOGI(TAG, "Waiting for HDMI signal lock...");
        }
        if (!sleep_interruptible_ms(CSI_VIDEO_POLL_SLICE_MS * CSI_VIDEO_POLL_SLICES)) {
            goto cleanup;
        }
    }
    if (stop_requested()) {
        goto cleanup;
    }

    // 2. Open HW JPEG encoder and MIPI CSI capture once.
    size_t jpeg_out_cap = csi_jpeg_calc_output_buf_size(s_cfg.hub_max_frame_bytes);
    if (jpeg_out_cap == 0u) {
        ESP_LOGE(TAG, "Invalid hub_max_frame_bytes=%u", (unsigned)s_cfg.hub_max_frame_bytes);
        goto cleanup;
    }
    csi_jpeg_config_t jpeg_cfg = {
        .quality = s_cfg.jpeg_quality,
        .max_output_bytes = jpeg_out_cap,
        .timeout_ms = 100,
    };
    esp_err_t err = csi_jpeg_open(&jpeg_cfg, &jpeg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "csi_jpeg_open failed: %s", esp_err_to_name(err));
        goto cleanup;
    }

    csi_capture_config_t cap_cfg = {
        .width = active_w,
        .height = active_h,
        .max_width = s_cfg.max_width,
        .max_height = s_cfg.max_height,
        .lane_bit_rate_mbps = s_cfg.lane_bit_rate_mbps,
        .data_lanes = s_cfg.data_lanes,
        .skip_frames = 1u,
        .create_isp_bypass = s_cfg.create_isp_bypass,
    };
    err = csi_capture_open(&cap_cfg, &cap);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "csi_capture_open failed: %s", esp_err_to_name(err));
        goto cleanup;
    }

    __atomic_store_n(&s_active_cap, cap, __ATOMIC_SEQ_CST);
    err = csi_capture_start(cap);
    if (err != ESP_OK) {
        __atomic_store_n(&s_active_cap, NULL, __ATOMIC_SEQ_CST);
        ESP_LOGE(TAG, "csi_capture_start failed: %s", esp_err_to_name(err));
        goto cleanup;
    }

    csi_video_seq_write(&s_seq_status, KVM_VIDEO_SYNC_LOCKED, active_w, active_h);
    ESP_LOGI(TAG, "Pipeline running: %" PRIu32 "x%" PRIu32 " RGB888 -> HW JPEG (q=%u, YUV420) -> Frame Hub "
                  "(hub_max=%u B, enc_buf=%u B, free_spiram=%u B, free_internal=%u B)",
             active_w, active_h, (unsigned)s_cfg.jpeg_quality,
             (unsigned)s_cfg.hub_max_frame_bytes, (unsigned)csi_jpeg_output_capacity(jpeg),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));

    // 3. Acquire -> HW JPEG encode -> release CSI buffer -> publish to Frame Hub.
    csi_video_pub_stats_t pub_stats;
    csi_video_pub_stats_init(&pub_stats);
    int64_t win_start_us = esp_timer_get_time();

    while (!stop_requested()) {
        const uint8_t *raw_data = NULL;
        size_t raw_len = 0u;
        int buf_idx = -1;

        err = csi_capture_acquire(cap, CSI_VIDEO_ACQUIRE_WAIT_MS, &raw_data, &raw_len, &buf_idx);
        if (err != ESP_OK) {
            // Reflect transient cable unplug in sync state; P2b-7 (#68) adds full mode-change controller recreation.
            uint32_t cur_w = 0u, cur_h = 0u;
            if (tc358743_get_active_size(s_tc_dev, &cur_w, &cur_h) != ESP_OK) {
                csi_video_status_t st;
                csi_video_seq_read(&s_seq_status, CSI_VIDEO_SEQ_DEFAULT_RETRIES, &st);
                if (st.state != KVM_VIDEO_SYNC_NO_SIGNAL) {
                    csi_video_seq_write(&s_seq_status, KVM_VIDEO_SYNC_NO_SIGNAL, 0u, 0u);
                    ESP_LOGW(TAG, "HDMI sync lost");
                }
            }
            continue;
        }

        csi_video_status_t cur_st;
        csi_video_seq_read(&s_seq_status, CSI_VIDEO_SEQ_DEFAULT_RETRIES, &cur_st);
        if (cur_st.state != KVM_VIDEO_SYNC_LOCKED) {
            csi_video_seq_write(&s_seq_status, KVM_VIDEO_SYNC_LOCKED, active_w, active_h);
            ESP_LOGI(TAG, "HDMI sync restored: %" PRIu32 "x%" PRIu32, active_w, active_h);
        }

        const uint8_t *jpeg_data = NULL;
        size_t jpeg_len = 0u;
        int64_t t_enc0 = esp_timer_get_time();
        esp_err_t enc_err = csi_jpeg_encode(jpeg, raw_data, raw_len,
                                            active_w, active_h, &jpeg_data, &jpeg_len);
        int64_t t_enc1 = esp_timer_get_time();

        // Return the PSRAM buffer to the CSI ping-pong pool immediately after DMA2D finishes reading it.
        (void)csi_capture_release(cap, buf_idx);

        uint32_t enc_us = (t_enc1 > t_enc0) ? (uint32_t)(t_enc1 - t_enc0) : 0u;
        if (enc_err != ESP_OK) {
            csi_video_pub_stats_on_error(&pub_stats);
            if (pub_stats.encode_errors <= 5u || (pub_stats.encode_errors % 100u) == 0u) {
                ESP_LOGW(TAG, "HW JPEG encode failed (count=%" PRIu64 "): %s",
                         pub_stats.encode_errors, esp_err_to_name(enc_err));
            }
        } else {
            csi_jpeg_frame_err_t ferr = csi_jpeg_check_frame(
                jpeg_data, jpeg_len, s_cfg.hub_max_frame_bytes);
            if (ferr == CSI_JPEG_FRAME_OK) {
                // Single-publisher contract (docs/INTERFACES.md §1.2).
                video_frame_t *dst = frame_hub_begin_publish(s_hub, jpeg_len);
                if (dst) {
                    memcpy(dst->data, jpeg_data, jpeg_len);
                    uint64_t ts_ms = (uint64_t)(esp_timer_get_time() / 1000);
                    frame_hub_commit(s_hub, dst, jpeg_len, active_w, active_h, ts_ms);
                    csi_video_pub_stats_on_published(&pub_stats, jpeg_len, enc_us);
                } else {
                    // Pool is busy (all buffers held by slow viewers); frame_hub already incremented dropped_frames.
                    csi_video_pub_stats_on_pool_busy(&pub_stats, jpeg_len, enc_us);
                }
            } else if (ferr == CSI_JPEG_FRAME_ERR_OVERSIZE) {
                // INTERFACES.md §1.2(b): frame_hub does NOT count oversize frames in dropped_frames;
                // we count and log them here with the exact byte size.
                csi_video_pub_stats_on_oversize(&pub_stats, jpeg_len, enc_us);
                if (pub_stats.oversize_drops <= 5u || (pub_stats.oversize_drops % 60u) == 0u) {
                    ESP_LOGW(TAG, "Dropped oversize JPEG: %u bytes > hub max %u bytes (count=%" PRIu64 ")",
                             (unsigned)jpeg_len, (unsigned)s_cfg.hub_max_frame_bytes,
                             pub_stats.oversize_drops);
                }
            } else {
                csi_video_pub_stats_on_error(&pub_stats);
            }
        }

        int64_t now_us = esp_timer_get_time();
        if (now_us - win_start_us >= CSI_VIDEO_STATS_PERIOD_US) {
            log_periodic_stats(&pub_stats, cap, active_w, active_h, now_us - win_start_us);
            csi_video_pub_stats_reset_window(&pub_stats);
            win_start_us = now_us;
        }
    }

cleanup:
    // Stop CSI DMA BEFORE releasing any PSRAM buffers (docs/P2B_TC358743_REFERENCE.md §7 item 13).
    if (cap) {
        (void)csi_capture_stop(cap);
        __atomic_store_n(&s_active_cap, NULL, __ATOMIC_SEQ_CST);
        (void)csi_capture_close(cap);
        cap = NULL;
    }
    if (jpeg) {
        (void)csi_jpeg_close(jpeg);
        jpeg = NULL;
    }
    csi_video_seq_write(&s_seq_status, KVM_VIDEO_SYNC_UNKNOWN, 0u, 0u);
    s_task_handle = NULL;
    if (s_done_sem) {
        xSemaphoreGive(s_done_sem);
    }
    vTaskDelete(NULL);
}

void csi_video_source_configure(tc358743_handle_t tc_dev, const csi_video_source_cfg_t *cfg)
{
    if (!tc_dev || !cfg) {
        ESP_LOGE(TAG, "csi_video_source_configure: invalid argument");
        return;
    }
    s_tc_dev = tc_dev;
    s_cfg = *cfg;
    if (s_cfg.jpeg_quality < 1u || s_cfg.jpeg_quality > 100u) {
        s_cfg.jpeg_quality = 70u;
    }
    if (s_cfg.lane_bit_rate_mbps <= 0) {
        s_cfg.lane_bit_rate_mbps = 972;
    }
    if (s_cfg.data_lanes == 0u) {
        s_cfg.data_lanes = 2u;
    }
    csi_video_seq_init(&s_seq_status);
    s_configured = true;
}

static int csi_video_source_start(const video_source_t *source, frame_hub_t *hub)
{
    (void)source;
    if (!hub || !s_configured || !s_tc_dev) {
        ESP_LOGE(TAG, "Cannot start csi_video_source: hub=%p configured=%d tc_dev=%p",
                 (void *)hub, (int)s_configured, (void *)s_tc_dev);
        return -1;
    }
    if (s_task_handle != NULL) {
        return 0;
    }

    if (!s_done_sem) {
        s_done_sem = xSemaphoreCreateBinary();
        if (!s_done_sem) {
            return -1;
        }
    }
    // Drain any stale completion token from an earlier stop().
    (void)xSemaphoreTake(s_done_sem, 0);

    if (!s_shutdown_registered) {
        if (esp_register_shutdown_handler(csi_video_shutdown_handler) == ESP_OK) {
            s_shutdown_registered = true;
        }
    }

    s_hub = hub;
    csi_video_seq_init(&s_seq_status);
    __atomic_store_n(&s_stop_requested, 0, __ATOMIC_SEQ_CST);

    if (xTaskCreate(csi_video_task, "csi_video", CSI_VIDEO_TASK_STACK,
                    NULL, CSI_VIDEO_TASK_PRIO, &s_task_handle) != pdPASS) {
        s_task_handle = NULL;
        return -1;
    }
    return 0;
}

static void csi_video_source_stop(const video_source_t *source)
{
    (void)source;
    if (!s_task_handle) {
        return;
    }
    __atomic_store_n(&s_stop_requested, 1, __ATOMIC_SEQ_CST);
    if (s_done_sem) {
        if (xSemaphoreTake(s_done_sem, pdMS_TO_TICKS(CSI_VIDEO_STOP_WAIT_MS)) != pdTRUE) {
            ESP_LOGW(TAG, "Timed out waiting for csi_video task to stop");
        }
    }
}

static const video_source_t s_csi_video_source = {
    .name = "csi",
    .start = csi_video_source_start,
    .stop = csi_video_source_stop,
    .ctx = NULL,
};

const video_source_t *csi_video_source_get(void)
{
    return &s_csi_video_source;
}

void csi_video_source_get_status(csi_video_status_t *out)
{
    if (!out) {
        return;
    }
    if (!s_configured) {
        out->state = KVM_VIDEO_SYNC_UNKNOWN;
        out->width = 0u;
        out->height = 0u;
        return;
    }
    (void)csi_video_seq_read(&s_seq_status, CSI_VIDEO_SEQ_DEFAULT_RETRIES, out);
}

kvm_video_sync_state_t csi_video_source_get_sync_state(void)
{
    csi_video_status_t st;
    csi_video_source_get_status(&st);
    return st.state;
}

void csi_video_source_get_mode(uint32_t *width, uint32_t *height)
{
    csi_video_status_t st;
    csi_video_source_get_status(&st);
    if (width) {
        *width = st.width;
    }
    if (height) {
        *height = st.height;
    }
}
