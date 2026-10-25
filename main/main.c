#include <stdio.h>
#include "esp_chip_info.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"

// If not enabled, define it to 0 so `if (CONFIG_TC358743_INIT)` doesn't fail compilation
#ifndef CONFIG_TC358743_INIT
#define CONFIG_TC358743_INIT 0
#endif

#ifndef CONFIG_NET_ETH_INIT
#define CONFIG_NET_ETH_INIT 0
#endif

// We deliberately include this unconditionally to ensure the header builds correctly regardless of toggle
#include "tc358743.h"
#include "net_eth.h"
#include "driver/i2c_master.h"
#include "frame_hub.h"
#include "test_pattern_source.h"
#include <stdlib.h>
#if CONFIG_WEB_SERVER_ENABLE
#include "web_server.h"
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
            if (web_server_start() == ESP_OK) {
                ESP_LOGI(TAG, "Web server started successfully");
            } else {
                ESP_LOGE(TAG, "Web server failed to start");
            }
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

        // TODO(hw-verify): PSRAM configuration is pending hardware verification
        frame_hub_t* hub = frame_hub_create(&hub_config);
        if (hub) {
            ESP_LOGI(TAG, "Starting test pattern source...");
            test_pattern_source_start(hub);
        } else {
            ESP_LOGE(TAG, "Failed to create frame hub");
        }
    }

    // Use a standard C `if` to guard the body, as specifically requested
    if (CONFIG_TC358743_INIT) {
        ESP_LOGI(TAG, "Starting TC358743 initialization...");

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
            tc358743_dump_diagnostics(tc_dev);
            // tc358743_poll_interrupts(tc_dev);
        } else {
            ESP_LOGE(TAG, "Failed to initialize TC358743");
        }
    }
}
