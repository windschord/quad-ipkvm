#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "hid_report.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Callback invoked when USB HID device mounts or resumes on the host bus.
 *
 * Called from TinyUSB task context (NOT inside hid_sink_t methods and NOT holding
 * any web_server mutex lock order inversion: web_server_notify_usb_mounted() is safe here).
 */
typedef void (*usb_hid_mounted_cb_t)(void);

/**
 * @brief Initialize TinyUSB composite HID device on ESP32-P4 OTG_HS.
 *
 * @param on_mounted Optional callback invoked on mount/resume events.
 * @return ESP_OK on success (or ESP_ERR_NOT_SUPPORTED if disabled via Kconfig).
 */
esp_err_t usb_hid_init(usb_hid_mounted_cb_t on_mounted);

/**
 * @brief Return the non-blocking @ref hid_sink_t wired to TinyUSB HID interfaces.
 *
 * @return Pointer to static hid_sink_t after successful usb_hid_init(), else NULL.
 */
const hid_sink_t *usb_hid_get_sink(void);

/**
 * @brief Assert or release USB soft-disconnect (DCTL.SFTDISCON) on OTG_HS.
 */
esp_err_t usb_hid_set_connected(bool connected);

#ifdef __cplusplus
}
#endif
