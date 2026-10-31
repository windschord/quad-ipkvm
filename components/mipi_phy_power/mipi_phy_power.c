#include "mipi_phy_power.h"
#include <stdio.h>

int mipi_phy_power_validate(int chan_id, int voltage_mv) {
    if (chan_id != 3) {
        return -1;
    }
    if (voltage_mv < 2250 || voltage_mv > 2750) {
        return -1;
    }
    return 0;
}

#ifdef ESP_PLATFORM
#include "esp_log.h"
#include "sdkconfig.h"
#include "esp_ldo_regulator.h"

static const char *TAG = "mipi_phy_power";

#if CONFIG_MIPI_PHY_POWER_ENABLE
static esp_ldo_channel_handle_t s_ldo_chan = NULL;
static int s_is_enabled = 0;
#endif

esp_err_t mipi_phy_power_enable(void) {
#if CONFIG_MIPI_PHY_POWER_ENABLE
    if (s_is_enabled) {
        return ESP_OK; // すでに有効化されている
    }

    int chan_id = CONFIG_MIPI_PHY_LDO_CHAN;
    int voltage_mv = CONFIG_MIPI_PHY_LDO_MV;

    if (mipi_phy_power_validate(chan_id, voltage_mv) < 0) {
        ESP_LOGE(TAG, "Validation failed: chan_id=%d, voltage_mv=%d", chan_id, voltage_mv);
        return ESP_ERR_INVALID_ARG;
    }

    esp_ldo_channel_config_t ldo_cfg = {
        .chan_id = chan_id,
        .voltage_mv = voltage_mv,
        .flags.adjustable = 0,
        .flags.owned_by_hw = 0
    };

    // esp_ldo_acquire_channel() はすでに他のユーザーが違う設定で利用していると失敗する。
    // (esp_ldo_regulator.h / .c のソースで確認。esp_ldo_acquire_channel() の実装により保証される)
    esp_err_t ret = esp_ldo_acquire_channel(&ldo_cfg, &s_ldo_chan);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to acquire LDO channel %d: %s", chan_id, esp_err_to_name(ret));
        return ret;
    }

    s_is_enabled = 1;
    ESP_LOGI(TAG, "Enabled PHY power: chan_id=%d, voltage_mv=%d", chan_id, voltage_mv);
    return ESP_OK;
#else
    ESP_LOGI(TAG, "disabled by Kconfig");
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

void mipi_phy_power_disable(void) {
#if CONFIG_MIPI_PHY_POWER_ENABLE
    if (!s_is_enabled) {
        return;
    }

    if (s_ldo_chan) {
        esp_ldo_release_channel(s_ldo_chan);
        s_ldo_chan = NULL;
    }
    s_is_enabled = 0;
    ESP_LOGI(TAG, "Disabled PHY power: chan_id=%d, voltage_mv=%d", CONFIG_MIPI_PHY_LDO_CHAN, CONFIG_MIPI_PHY_LDO_MV);
#endif
}
#endif
