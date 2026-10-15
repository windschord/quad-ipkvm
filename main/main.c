#include <stdio.h>
#include "esp_chip_info.h"
#include "esp_log.h"

static const char *TAG = "app_main";

void app_main(void)
{
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);

    ESP_LOGI(TAG, "ESP32-P4 chip revision: v%d.%d", chip_info.revision / 100, chip_info.revision % 100);
}
