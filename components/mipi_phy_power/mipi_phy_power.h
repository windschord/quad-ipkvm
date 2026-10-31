#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// 純 C の検証関数。host-test可能
int mipi_phy_power_validate(int chan_id, int voltage_mv);

#ifdef ESP_PLATFORM
#include "esp_err.h"
esp_err_t mipi_phy_power_enable(void);
void mipi_phy_power_disable(void);
#endif

#ifdef __cplusplus
}
#endif
