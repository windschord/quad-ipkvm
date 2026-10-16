#include <stdio.h>
#include "esp_chip_info.h"
#include "esp_log.h"
#include "sdkconfig.h"

// If not enabled, define it to 0 so `if (CONFIG_TC358743_INIT)` doesn't fail compilation
#ifndef CONFIG_TC358743_INIT
#define CONFIG_TC358743_INIT 0
#endif

// We deliberately include this unconditionally to ensure the header builds correctly regardless of toggle
#include "tc358743.h"
#include "driver/i2c_master.h"

static const char *TAG = "app_main";

void app_main(void)
{
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);

    ESP_LOGI(TAG, "ESP32-P4 chip revision: v%d.%d", chip_info.revision / 100, chip_info.revision % 100);

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
            .refclk_hz = 27000000, // TODO(hw-verify): X630 27MHz?
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
