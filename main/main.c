#include <stdio.h>
#include "esp_chip_info.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"

// If not enabled, define it to 0 so `if (CONFIG_TC358743_INIT)` doesn't fail compilation

#ifndef CONFIG_TC358743_START_CSI
#define CONFIG_TC358743_START_CSI 0
#endif
#ifndef CONFIG_TC358743_CSI_CONTINUOUS_CLOCK
#define CONFIG_TC358743_CSI_CONTINUOUS_CLOCK 0
#endif
#ifndef CONFIG_TC358743_CSI_FORMAT_UYVY
#define CONFIG_TC358743_CSI_FORMAT_UYVY 0
#endif
#ifndef CONFIG_TC358743_CSI_LANE_RATE_MBPS
#define CONFIG_TC358743_CSI_LANE_RATE_MBPS 972
#endif
#ifndef CONFIG_TC358743_INIT
#define CONFIG_TC358743_INIT 0
#endif
#ifndef CONFIG_MIPI_PHY_POWER_ENABLE
#define CONFIG_MIPI_PHY_POWER_ENABLE 0
#endif
#ifndef CONFIG_VIDEO_SOURCE_CSI
#define CONFIG_VIDEO_SOURCE_CSI 0
#endif
#ifndef CONFIG_JPEG_ENCODE_QUALITY
#define CONFIG_JPEG_ENCODE_QUALITY 70
#endif
#ifndef CONFIG_CSI_CAPTURE_MAX_WIDTH
#define CONFIG_CSI_CAPTURE_MAX_WIDTH 1920
#endif
#ifndef CONFIG_CSI_CAPTURE_MAX_HEIGHT
#define CONFIG_CSI_CAPTURE_MAX_HEIGHT 1080
#endif
#ifndef CONFIG_CSI_CAPTURE_ISP_BYPASS
#define CONFIG_CSI_CAPTURE_ISP_BYPASS 1
#endif

#ifndef CONFIG_NET_ETH_INIT
#define CONFIG_NET_ETH_INIT 0
#endif

// We deliberately include this unconditionally to ensure the header builds correctly regardless of toggle
#include "tc358743.h"
#include "mipi_phy_power.h"
#include "csi_capture.h"
#include "csi_video_source.h"
#include <inttypes.h>
#include "net_eth.h"
#include "driver/i2c_master.h"
#include "frame_hub.h"
#include "test_pattern_source.h"
#include <stdlib.h>
#if CONFIG_WEB_SERVER_ENABLE
#include "web_server.h"
#endif
#if CONFIG_USB_DEVICE_ENABLE
#include "usb_hid.h"
#endif
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "app_main";

static void* app_alloc(size_t size, void* ctx) {
    (void)ctx;
    void* ptr = NULL;
#ifdef CONFIG_FRAME_HUB_PREFER_SPIRAM
#if CONFIG_FRAME_HUB_PREFER_SPIRAM
    ptr = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#endif
#endif
    if (!ptr) {
        ptr = malloc(size);
    }
    return ptr;
}

static void app_free(void* ptr, void* ctx) {
    (void)ctx;
    free(ptr);
}

static void app_lock(void* ctx) {
    xSemaphoreTake((SemaphoreHandle_t)ctx, portMAX_DELAY);
}

static void app_unlock(void* ctx) {
    xSemaphoreGive((SemaphoreHandle_t)ctx);
}

#if CONFIG_CSI_CAPTURE_TEST
// P2b-4 bring-up: receive RGB888 frames into PSRAM and log raw-data statistics.
// Memory order is expected to be B,G,R per pixel on rev v1.3 [参照: docs/P2B_TC358743_REFERENCE.md 4-2].
static void log_frame_stats(int n, const uint8_t* p, size_t len, uint32_t width) {
    uint64_t sum[3] = {0, 0, 0};
    uint32_t samples = 0, nonzero = 0;
    uint8_t mn = 0xFF, mx = 0;
    uint32_t checksum = 0;
    for (size_t i = 0; i + 2 < len; i += 3) {
        uint8_t b0 = p[i], b1 = p[i + 1], b2 = p[i + 2];
        checksum = checksum * 31u + b0 + b1 + b2;
        if (b0 | b1 | b2) nonzero++;
        if ((i / 3) % 101 == 0) {  // sparse sample for channel means
            sum[0] += b0; sum[1] += b1; sum[2] += b2;
            samples++;
            uint8_t lo = b0 < b1 ? (b0 < b2 ? b0 : b2) : (b1 < b2 ? b1 : b2);
            uint8_t hi = b0 > b1 ? (b0 > b2 ? b0 : b2) : (b1 > b2 ? b1 : b2);
            if (lo < mn) mn = lo;
            if (hi > mx) mx = hi;
        }
    }
    size_t mid = ((len / 3) / 2) * 3;  // centre-ish pixel
    ESP_LOGI(TAG, "frame %d: %u bytes, non-zero px %"PRIu32"/%u, mean[b0,b1,b2]=%u,%u,%u (n=%"PRIu32"), min %u max %u, "
             "px0=%02x%02x%02x px1=%02x%02x%02x mid=%02x%02x%02x row1px0=%02x%02x%02x, hash %08"PRIx32,
             n, (unsigned)len, nonzero, (unsigned)(len / 3),
             samples ? (unsigned)(sum[0] / samples) : 0u, samples ? (unsigned)(sum[1] / samples) : 0u,
             samples ? (unsigned)(sum[2] / samples) : 0u, samples, mn, mx,
             p[0], p[1], p[2], p[3], p[4], p[5], p[mid], p[mid + 1], p[mid + 2],
             p[width * 3], p[width * 3 + 1], p[width * 3 + 2], checksum);
}

static void run_csi_capture_test(tc358743_handle_t tc_dev) {
    uint32_t w = 0, h = 0;
    esp_err_t err = tc358743_get_active_size(tc_dev, &w, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "CSI capture test skipped: no active size (%s)", esp_err_to_name(err));
        return;
    }
    csi_capture_config_t cfg = {
        .width = w,
        .height = h,
        .max_width = CONFIG_CSI_CAPTURE_MAX_WIDTH,
        .max_height = CONFIG_CSI_CAPTURE_MAX_HEIGHT,
        .lane_bit_rate_mbps = CONFIG_TC358743_CSI_LANE_RATE_MBPS,
        .data_lanes = 2,
        .skip_frames = 1,           // the first completion may start mid-frame
        .create_isp_bypass = CONFIG_CSI_CAPTURE_ISP_BYPASS,
        .pixel_format = CONFIG_TC358743_CSI_FORMAT_UYVY ? CSI_PIXEL_FORMAT_UYVY : CSI_PIXEL_FORMAT_RGB888,
    };
    csi_capture_handle_t cap = NULL;
    err = csi_capture_open(&cfg, &cap);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "csi_capture_open failed: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "Free PSRAM after capture buffers: %u bytes",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    err = csi_capture_start(cap);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "csi_capture_start failed: %s", esp_err_to_name(err));
        csi_capture_close(cap);
        return;
    }
    csi_capture_log_hw_state(cap, "after start");

    int got = 0, timeouts = 0;
    for (int n = 0; n < CONFIG_CSI_CAPTURE_TEST_FRAMES && timeouts < 3; n++) {
        const uint8_t* data = NULL;
        size_t len = 0;
        int idx = -1;
        err = csi_capture_acquire(cap, 1000, &data, &len, &idx);
        if (err != ESP_OK) {
            timeouts++;
            csi_capture_stats_t st;
            csi_capture_get_stats(cap, &st);
            ESP_LOGW(TAG, "No frame within 1000 ms (%s); DMA completions so far: %"PRIu32,
                     esp_err_to_name(err), st.completed);
            csi_capture_log_hw_state(cap, "timeout");
            continue;
        }
        got++;
        if (got <= 3 || got == CONFIG_CSI_CAPTURE_TEST_FRAMES) {
            log_frame_stats(got, data, len, w);
        }
        csi_capture_release(cap, idx);
    }

    csi_capture_log_hw_state(cap, "before stop");
    csi_capture_stats_t st;
    csi_capture_get_stats(cap, &st);
    double fps = 0.0;
    if (st.completed > 1 && st.last_frame_us > st.first_frame_us) {
        fps = (double)(st.completed - 1) * 1e6 / (double)(st.last_frame_us - st.first_frame_us);
    }
    ESP_LOGI(TAG, "CSI capture test: %dx%d, acquired %d frames, DMA completions %"PRIu32", published %"PRIu32
             ", overwritten %"PRIu32", DMA rate %.2f fps",
             (int)st.width, (int)st.height, got, st.completed, st.published, st.overwritten, fps);

    // Stop the CSI DMA before anything else (it writes PSRAM continuously; see csi_capture.h).
    err = csi_capture_close(cap);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "csi_capture_close failed: %s", esp_err_to_name(err));
    }
}
#endif

void app_main(void)
{
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);

    ESP_LOGI(TAG, "ESP32-P4 chip revision: v%d.%d", chip_info.revision / 100, chip_info.revision % 100);

    esp_err_t netif_ret = esp_netif_init();
    if (netif_ret != ESP_OK && netif_ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_netif_init failed: %s", esp_err_to_name(netif_ret));
    }

    esp_err_t event_ret = esp_event_loop_create_default();
    if (event_ret != ESP_OK && event_ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_event_loop_create_default failed: %s", esp_err_to_name(event_ret));
    }

#if CONFIG_WEB_SERVER_ENABLE
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGE(TAG, "NVS flash initialization failed: %s. Not erasing NVS.", esp_err_to_name(ret));
    } else if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS flash init failed: %s", esp_err_to_name(ret));
    } else {
        if (netif_ret != ESP_OK && netif_ret != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "Web server not started because esp_netif_init failed");
        } else if (event_ret != ESP_OK && event_ret != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "Web server not started because esp_event_loop_create_default failed");
        } else {
            // Deferred web_server_start to be called after frame hub is created.
            // If frame hub fails, web_server_start is called with NULL.
        }
    }
#endif

    if (CONFIG_NET_ETH_INIT) {
        ESP_LOGI(TAG, "Starting Ethernet initialization...");
        esp_err_t err = net_eth_init();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "Ethernet initialization failed: %s", esp_err_to_name(err));
        }
    }

    frame_hub_t* hub = NULL;
    SemaphoreHandle_t hub_mutex = xSemaphoreCreateMutex();
    if (!hub_mutex) {
        ESP_LOGE(TAG, "Failed to create mutex for frame hub");
        // We can just continue to let the rest init if they don't depend on it
    } else {
        frame_hub_config_t hub_config = {
            .pool_size = CONFIG_FRAME_HUB_POOL_SIZE,
            .max_clients = 8,
            .max_frame_size = CONFIG_FRAME_HUB_MAX_FRAME_KB * 1024,
            .alloc_cb = app_alloc,
            .free_cb = app_free,
            .lock_cb = app_lock,
            .unlock_cb = app_unlock,
            .lock_ctx = (void*)hub_mutex
        };

        const char* prefer_spiram = "No";
#ifdef CONFIG_FRAME_HUB_PREFER_SPIRAM
#if CONFIG_FRAME_HUB_PREFER_SPIRAM
        prefer_spiram = "Yes";
#endif
#endif

        ESP_LOGI(TAG, "Creating frame hub: pool_size=%d, max_frame_size=%d KB, prefer_spiram=%s",
                 CONFIG_FRAME_HUB_POOL_SIZE, CONFIG_FRAME_HUB_MAX_FRAME_KB, prefer_spiram);
        ESP_LOGI(TAG, "Free 8-bit heap size: %d bytes", (int)heap_caps_get_free_size(MALLOC_CAP_8BIT));

        // Verified on hardware with CONFIG_FRAME_HUB_PREFER_SPIRAM=y (source: docs/HW_VERIFY.md D4):
        // the 6 allocations made through app_alloc during frame_hub_create all landed in external RAM.
        // Without PREFER_SPIRAM, app_alloc falls back to malloc() and the placement is not verified.
        hub = frame_hub_create(&hub_config);
        if (hub) {
            if (!CONFIG_VIDEO_SOURCE_CSI) {
                const video_source_t* vs = test_pattern_source_get();
                ESP_LOGI(TAG, "Starting video source '%s'...", vs->name);
                vs->start(vs, hub);
            }
        } else {
            ESP_LOGE(TAG, "Failed to create frame hub");
        }
#if CONFIG_WEB_SERVER_ENABLE
        if (netif_ret == ESP_OK || netif_ret == ESP_ERR_INVALID_STATE) {
            if (event_ret == ESP_OK || event_ret == ESP_ERR_INVALID_STATE) {
                if (web_server_start(hub) == ESP_OK) {
                    ESP_LOGI(TAG, "Web server started successfully");
                } else {
                    ESP_LOGE(TAG, "Web server failed to start");
                }
            }
        }
#endif
#if CONFIG_USB_DEVICE_ENABLE
        {
#if CONFIG_WEB_SERVER_ENABLE
            usb_hid_mounted_cb_t on_mounted = web_server_notify_usb_mounted;
#else
            usb_hid_mounted_cb_t on_mounted = NULL;
#endif
            esp_err_t usb_err = usb_hid_init(on_mounted);
            if (usb_err != ESP_OK) {
                ESP_LOGE(TAG, "usb_hid_init failed: %s", esp_err_to_name(usb_err));
            } else {
#if CONFIG_WEB_SERVER_ENABLE
                web_server_set_hid_sink(usb_hid_get_sink());
#endif
            }
        }
#endif
    }

    // Use a standard C `if` to guard the body, as specifically requested
    if (CONFIG_TC358743_INIT) {
        ESP_LOGI(TAG, "Starting TC358743 initialization...");

        if (CONFIG_MIPI_PHY_POWER_ENABLE) {
            esp_err_t power_err = mipi_phy_power_enable();
            if (power_err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to enable MIPI PHY power: %s", esp_err_to_name(power_err));
            }
        }

        i2c_master_bus_config_t i2c_bus_config = {
            .i2c_port = 0,
            .sda_io_num = 7, // Source: docs/ESP32-P4-ETH-schematic.pdf ESP_I2C_SDA
            .scl_io_num = 8, // Source: docs/ESP32-P4-ETH-schematic.pdf ESP_I2C_SCL
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .flags.enable_internal_pullup = false, // 2.2k pull-up exists on board
        };
        i2c_master_bus_handle_t bus_handle;
        if (i2c_new_master_bus(&i2c_bus_config, &bus_handle) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize I2C master bus");
            return;
        }

        tc358743_config_t config = {
            .i2c_bus = bus_handle,
            .i2c_addr = 0x0F,
            // X630 crystal is marked "27.000" (source: docs/HW_VERIFY.md E3).
            // TODO(hw-verify): actual crystal frequency and its REFCLK connection are unverified (check schematic)
            .refclk_hz = 27000000,
            .pll_prd = 0, // Unverified, triggers a warning and skips
            .pll_fbd = 0, // Unverified
            .enable_edid = true
        };

        tc358743_handle_t tc_dev = tc358743_init(&config);
        if (tc_dev) {
            if (CONFIG_TC358743_START_CSI) {
                tc358743_mode_t mode = {
                    .lane_rate_mbps = CONFIG_TC358743_CSI_LANE_RATE_MBPS,
                    .lanes = 2,
                    .format = CONFIG_TC358743_CSI_FORMAT_UYVY ? TC358743_FORMAT_UYVY : TC358743_FORMAT_RGB888,
                    .continuous_clock = CONFIG_TC358743_CSI_CONTINUOUS_CLOCK
                };
                bool locked = false;
                ESP_LOGI(TAG, "Starting TC358743 CSI output...");
                esp_err_t start_err = tc358743_start_csi(tc_dev, &mode, &locked);
                if (start_err == ESP_OK) {
                    ESP_LOGI(TAG, "TC358743 start sequence complete. HDMI locked: %s", locked ? "Yes" : "No");
                } else {
                    ESP_LOGE(TAG, "TC358743 start sequence failed.");
                }
            }
            // 診断（読み出しのみ）。START_CSI=n でも出す（段階 2 の確認用）。
            tc358743_dump_diagnostics(tc_dev);

#if CONFIG_CSI_CAPTURE_TEST
            run_csi_capture_test(tc_dev);
            tc358743_dump_diagnostics(tc_dev);
#endif

            if (CONFIG_VIDEO_SOURCE_CSI && hub) {
                csi_video_source_cfg_t vs_cfg = {
                    .max_width = CONFIG_CSI_CAPTURE_MAX_WIDTH,
                    .max_height = CONFIG_CSI_CAPTURE_MAX_HEIGHT,
                    .lane_bit_rate_mbps = CONFIG_TC358743_CSI_LANE_RATE_MBPS,
                    .data_lanes = 2,
                    .create_isp_bypass = CONFIG_CSI_CAPTURE_ISP_BYPASS,
                    .jpeg_quality = CONFIG_JPEG_ENCODE_QUALITY,
                    .hub_max_frame_bytes = (size_t)CONFIG_FRAME_HUB_MAX_FRAME_KB * 1024u,
                    .pixel_format = CONFIG_TC358743_CSI_FORMAT_UYVY ? TC358743_FORMAT_UYVY : TC358743_FORMAT_RGB888,
                };
                csi_video_source_configure(tc_dev, &vs_cfg);
                const video_source_t *vs = csi_video_source_get();
                ESP_LOGI(TAG, "Starting video source '%s'...", vs->name);
                if (vs->start(vs, hub) != 0) {
                    ESP_LOGE(TAG, "Failed to start video source '%s'", vs->name);
                }
            }

            // tc358743_poll_interrupts(tc_dev);
        } else {
            ESP_LOGE(TAG, "Failed to initialize TC358743");
        }
    }
}
