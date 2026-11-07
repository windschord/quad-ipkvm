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

static uint64_t now_monotonic_ms(void)
{
    int64_t us = esp_timer_get_time();
    return (us > 0) ? (uint64_t)(us / 1000) : 0u;
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

static void ensure_no_signal_status(void)
{
    csi_video_status_t cur;
    csi_video_seq_read(&s_seq_status, CSI_VIDEO_SEQ_DEFAULT_RETRIES, &cur);
    if (cur.state != KVM_VIDEO_SYNC_NO_SIGNAL) {
        csi_video_seq_write(&s_seq_status, KVM_VIDEO_SYNC_NO_SIGNAL, 0u, 0u);
    }
}

static void sample_hdmi_mode(uint64_t now_ms, size_t max_buf_cap,
                             csi_mode_sample_t *out, csi_geom_result_t *out_geom_err)
{
    memset(out, 0, sizeof(*out));
    out->now_ms = now_ms;
    if (out_geom_err) {
        *out_geom_err = CSI_GEOM_OK;
    }

    tc358743_signal_t sig;
    if (tc358743_get_signal(s_tc_dev, &sig) != ESP_OK) {
        return;
    }

    out->ddc5v = sig.ddc5v;
    out->sync_locked = (sig.tmds && sig.sync);
    out->width = sig.width;
    out->height = sig.height;

    if (out->sync_locked) {
        size_t raw_bytes = 0u;
        csi_geom_result_t g = csi_capture_check_geometry(
            sig.width, sig.height, 24u,
            s_cfg.max_width, s_cfg.max_height, max_buf_cap, &raw_bytes);
        out->geom_ok = (g == CSI_GEOM_OK);
        if (out_geom_err) {
            *out_geom_err = g;
        }
    }
}

static void log_periodic_stats(const csi_video_pub_stats_t *pub,
                               const csi_mode_tracker_t *tracker,
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
             "hub clients=%d dropped=%" PRIu64 " | csi done=%" PRIu32 " ovw=%" PRIu32 " | "
             "modes=%" PRIu32 " nudges=%" PRIu32 " stalls=%" PRIu32 " | stack_free=%u B",
             width, height, enc_fps, avg_enc_ms, max_enc_ms,
             (unsigned)pub->win_min_jpeg_bytes, (unsigned)avg_bytes,
             (unsigned)pub->win_max_jpeg_bytes, (unsigned)pub->peak_jpeg_bytes,
             pub->published, pub->pool_busy_drops, pub->oversize_drops, pub->encode_errors,
             hub_st.active_clients, hub_st.dropped_frames,
             cap_st.completed, cap_st.overwritten,
             tracker->mode_switches, tracker->hpd_nudges, tracker->stall_recoveries,
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
}

static void csi_video_task(void *arg)
{
    (void)arg;
    csi_capture_handle_t cap = NULL;
    csi_jpeg_handle_t jpeg = NULL;

    // 1. Pre-allocate HW JPEG output buffer and 2x max-mode CSI PSRAM buffers ONCE at startup
    //    so resolution switches never fragment PSRAM (docs/P2B_PLAN.md P2b-7).
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
        .width = 0u,
        .height = 0u,
        .max_width = s_cfg.max_width,
        .max_height = s_cfg.max_height,
        .lane_bit_rate_mbps = s_cfg.lane_bit_rate_mbps,
        .data_lanes = s_cfg.data_lanes,
        .skip_frames = 1u,
        .create_isp_bypass = s_cfg.create_isp_bypass,
    };
    err = csi_capture_open_pool(&cap_cfg, &cap);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "csi_capture_open_pool failed: %s", esp_err_to_name(err));
        goto cleanup;
    }
    __atomic_store_n(&s_active_cap, cap, __ATOMIC_SEQ_CST);

    csi_capture_stats_t pool_st;
    csi_capture_get_stats(cap, &pool_st);
    size_t max_buf_cap = pool_st.buffer_bytes;

    csi_mode_tracker_t tracker;
    csi_mode_tracker_init(&tracker);
    csi_video_pub_stats_t pub_stats;
    csi_video_pub_stats_init(&pub_stats);

    int64_t win_start_us = esp_timer_get_time();
    uint64_t last_mode_poll_ms = 0u;
    bool force_mode_poll = true;
    uint32_t reject_logs = 0u;

    ESP_LOGI(TAG, "CSI video source ready: max=%" PRIu32 "x%" PRIu32 " (2x%u B PSRAM), "
                  "enc_buf=%u B, hub_max=%u B, free_spiram=%u B",
             s_cfg.max_width, s_cfg.max_height, (unsigned)max_buf_cap,
             (unsigned)csi_jpeg_output_capacity(jpeg), (unsigned)s_cfg.hub_max_frame_bytes,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    // 2. Unified mode-tracking + capture/encode/publish loop (P2b-6 + P2b-7).
    while (!stop_requested()) {
        uint64_t now_ms = now_monotonic_ms();

        if (!tracker.streaming) {
            csi_mode_sample_t sample;
            csi_geom_result_t gerr = CSI_GEOM_OK;
            sample_hdmi_mode(now_ms, max_buf_cap, &sample, &gerr);
            csi_mode_action_t act = csi_mode_tracker_step(&tracker, &sample);

            if (act == CSI_MODE_ACT_START_MODE) {
                uint32_t w = tracker.active_w;
                uint32_t h = tracker.active_h;
                esp_err_t rearm_err = tc358743_rearm_csi(s_tc_dev, w);
                if (rearm_err != ESP_OK) {
                    ESP_LOGW(TAG, "tc358743_rearm_csi(%" PRIu32 ") failed: %s",
                             w, esp_err_to_name(rearm_err));
                }
                err = csi_capture_reconfigure(cap, w, h);
                if (err == ESP_OK) {
                    err = csi_capture_start(cap);
                }
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "Failed to start CSI for %" PRIu32 "x%" PRIu32 ": %s",
                             w, h, esp_err_to_name(err));
                    (void)csi_capture_stop(cap);
                    csi_mode_tracker_abort_start(&tracker);
                    ensure_no_signal_status();
                    if (!sleep_interruptible_ms(CSI_MODE_POLL_INTERVAL_MS)) {
                        break;
                    }
                    continue;
                }

                reject_logs = 0u;
                last_mode_poll_ms = now_monotonic_ms();
                force_mode_poll = false;
                tracker.last_frame_ms = last_mode_poll_ms;
                csi_video_pub_stats_reset_window(&pub_stats);
                win_start_us = esp_timer_get_time();
                csi_video_seq_write(&s_seq_status, KVM_VIDEO_SYNC_LOCKED, w, h);

                ESP_LOGI(TAG, "Pipeline locked (#%" PRIu32 "): %" PRIu32 "x%" PRIu32
                              " RGB888 -> HW JPEG (q=%u, YUV420) -> Frame Hub "
                              "(free_spiram=%u B, free_internal=%u B)",
                         tracker.mode_switches, w, h, (unsigned)s_cfg.jpeg_quality,
                         (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                         (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
                continue;
            }

            ensure_no_signal_status();

            if (act == CSI_MODE_ACT_NUDGE_HPD) {
                ESP_LOGW(TAG, "DDC5V present without HDMI sync (nudge #%" PRIu32 "/%u); pulsing HPD...",
                         tracker.hpd_nudges, (unsigned)CSI_HPD_NUDGE_STEPS);
                esp_err_t nerr = tc358743_nudge_hpd(s_tc_dev);
                if (nerr != ESP_OK) {
                    ESP_LOGW(TAG, "tc358743_nudge_hpd failed: %s", esp_err_to_name(nerr));
                }
                continue;
            }

            if (act == CSI_MODE_ACT_REJECT_OUT_OF_RANGE) {
                if ((reject_logs++ % 25u) == 0u) {
                    ESP_LOGW(TAG, "Rejected out-of-range HDMI mode %" PRIu32 "x%" PRIu32
                                  " (max %" PRIu32 "x%" PRIu32 ", geom_err=%d)",
                             sample.width, sample.height,
                             s_cfg.max_width, s_cfg.max_height, (int)gerr);
                }
            }

            if (!sleep_interruptible_ms(CSI_MODE_POLL_INTERVAL_MS)) {
                break;
            }
            continue;
        }

        // Streaming state: periodically verify HDMI sync & active resolution every 200 ms
        // (or immediately after an acquire timeout) so mode changes tear down CSI cleanly.
        if (force_mode_poll || (now_ms - last_mode_poll_ms) >= CSI_MODE_POLL_INTERVAL_MS) {
            last_mode_poll_ms = now_ms;
            force_mode_poll = false;

            csi_mode_sample_t sample;
            csi_geom_result_t gerr = CSI_GEOM_OK;
            sample_hdmi_mode(now_ms, max_buf_cap, &sample, &gerr);
            csi_mode_action_t act = csi_mode_tracker_step(&tracker, &sample);

            if (act == CSI_MODE_ACT_STOP_NO_SIGNAL) {
                (void)csi_capture_stop(cap);
                ensure_no_signal_status();
                ESP_LOGW(TAG, "HDMI sync lost (SYS_STATUS ddc5v=%d); CSI DMA stopped",
                         (int)sample.ddc5v);
                if (!sleep_interruptible_ms(CSI_MODE_POLL_INTERVAL_MS)) {
                    break;
                }
                continue;
            }
            if (act == CSI_MODE_ACT_STOP_MODE_CHANGED) {
                (void)csi_capture_stop(cap);
                ensure_no_signal_status();
                ESP_LOGI(TAG, "HDMI resolution change detected (-> candidate %" PRIu32 "x%" PRIu32
                              ", sample 1/%u); CSI DMA stopped",
                         sample.width, sample.height, (unsigned)CSI_MODE_STABLE_COUNT);
                if (!sleep_interruptible_ms(CSI_MODE_POLL_INTERVAL_MS)) {
                    break;
                }
                continue;
            }
            if (act == CSI_MODE_ACT_REJECT_OUT_OF_RANGE) {
                (void)csi_capture_stop(cap);
                ensure_no_signal_status();
                ESP_LOGW(TAG, "HDMI switched to out-of-range mode %" PRIu32 "x%" PRIu32
                              " (geom_err=%d); CSI DMA stopped",
                         sample.width, sample.height, (int)gerr);
                if (!sleep_interruptible_ms(CSI_MODE_POLL_INTERVAL_MS)) {
                    break;
                }
                continue;
            }

            if (csi_mode_tracker_check_stall(&tracker, now_ms) == CSI_MODE_ACT_RECOVER_STALL) {
                (void)csi_capture_stop(cap);
                ensure_no_signal_status();
                ESP_LOGW(TAG, "CSI frame stall (>= %u ms) while HDMI locked; pulsing HPD recovery (#%" PRIu32 ")...",
                         (unsigned)CSI_MODE_STALL_TIMEOUT_MS, tracker.stall_recoveries);
                (void)tc358743_nudge_hpd(s_tc_dev);
                continue;
            }
        }

        uint32_t active_w = tracker.active_w;
        uint32_t active_h = tracker.active_h;
        const uint8_t *raw_data = NULL;
        size_t raw_len = 0u;
        int buf_idx = -1;

        err = csi_capture_acquire(cap, CSI_VIDEO_ACQUIRE_WAIT_MS, &raw_data, &raw_len, &buf_idx);
        if (err != ESP_OK) {
            force_mode_poll = true;
            continue;
        }

        csi_mode_tracker_on_frame(&tracker, now_monotonic_ms());

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
                    csi_video_pub_stats_on_pool_busy(&pub_stats, jpeg_len, enc_us);
                }
            } else if (ferr == CSI_JPEG_FRAME_ERR_OVERSIZE) {
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
            log_periodic_stats(&pub_stats, &tracker, cap, active_w, active_h, now_us - win_start_us);
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
