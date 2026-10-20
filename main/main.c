#include <stdio.h>
#include <stdlib.h>
#include "esp_chip_info.h"
#include "esp_log.h"
#include "sdkconfig.h"

#ifndef CONFIG_TC358743_INIT
#define CONFIG_TC358743_INIT 0
#endif

#ifndef CONFIG_NET_ETH_INIT
#define CONFIG_NET_ETH_INIT 0
#endif

#include "tc358743.h"
#include "net_eth.h"
#include "driver/i2c_master.h"

#include "frame_hub.h"
#include "test_pattern_source.h"

static const char *TAG = "app_main";

// TODO(hw-verify): allocate from PSRAM if available
static void* idf_alloc(size_t size) { return malloc(size); }
static void idf_free(void* ptr) { free(ptr); }

void app_main(void)
{
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);

    ESP_LOGI(TAG, "ESP32-P4 chip revision: v%d.%d", chip_info.revision / 100, chip_info.revision % 100);

    frame_hub_config_t hub_config = {
        .alloc = idf_alloc,
        .free = idf_free,
        .pool_size = 3,
        .max_clients = 2,
        .max_frame_size = 512 * 1024
    };
    frame_hub_t* hub = frame_hub_create(&hub_config);
    if (hub) {
        test_pattern_source_start(hub);
        ESP_LOGI(TAG, "Test pattern source started.");
    } else {
        ESP_LOGE(TAG, "Failed to create frame hub.");
    }

    if (CONFIG_NET_ETH_INIT) {
        ESP_LOGI(TAG, "Starting Ethernet initialization...");
        esp_err_t err = net_eth_init();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "Ethernet initialization failed: %s", esp_err_to_name(err));
        }
    }

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
