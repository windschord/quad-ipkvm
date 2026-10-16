#include <stdio.h>
#include "esp_chip_info.h"
#include "esp_log.h"
#include "sdkconfig.h"

// Provide an empty mock configuration to allow CI compilation even if TC358743 component isn't strictly requested.
#if __has_include("tc358743.h")
#include "tc358743.h"
#endif

#ifndef CONFIG_TC358743_INIT
#define CONFIG_TC358743_INIT 0
#endif

static const char *TAG = "app_main";

void app_main(void)
{
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);

    ESP_LOGI(TAG, "ESP32-P4 chip revision: v%d.%d", chip_info.revision / 100, chip_info.revision % 100);

    // Call TC358743 initialization conditionally.
    // The if statement ensures compilation succeeds when config is n, provided the headers are included.
#if __has_include("tc358743.h")
    if (CONFIG_TC358743_INIT) {
        ESP_LOGI(TAG, "Starting TC358743 initialization...");

        tc358743_config_t config = {
            .i2c_bus = NULL, // Expected to be initialized elsewhere
            .i2c_addr = 0x0F,
            .refclk_hz = 27000000,
            .enable_edid = true
        };

        tc358743_handle_t tc_dev = tc358743_init(&config);
        if (tc_dev) {
            tc358743_dump_diagnostics(tc_dev);
            // In a real app, you would keep the handle and poll it
            // tc358743_deinit(tc_dev);
        } else {
            ESP_LOGE(TAG, "Failed to initialize TC358743");
        }
    }
#endif
}
