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
 * When VBUS sensing is enabled (CONFIG_USB_DEVICE_VBUS_SENSE_GPIO >= 0), boots
 * soft-disconnected and tracks target VBUS. If stage-2 FORCE_OFF
 * (tinyusb_driver_uninstall() + UTMI PHY stop) succeeds after a persistent
 * disconnect failure, the USB stack remains permanently uninstalled until
 * system reboot (returns ESP_ERR_INVALID_STATE).
 *
 * @param on_mounted Optional callback invoked on mount/resume events.
 * @return ESP_OK on success (or ESP_ERR_NOT_SUPPORTED if disabled via Kconfig).
 */
esp_err_t usb_hid_init(usb_hid_mounted_cb_t on_mounted);

/**
 * @brief Initialize TinyUSB composite HID (and optional MSC) device on ESP32-P4 OTG_HS.
 *
 * When @p msc_ready is true and CONFIG_USB_MSC_ENABLE is enabled, exposes the
 * 107-byte HID+MSC descriptors; otherwise falls back to the 84-byte HID-only
 * descriptors so HID remains operational even if MSC initialization fails.
 */
esp_err_t usb_hid_init_ex(usb_hid_mounted_cb_t on_mounted, bool msc_ready);

/**
 * @brief Return the non-blocking @ref hid_sink_t wired to TinyUSB HID interfaces.
 *
 * @return Pointer to static hid_sink_t after successful usb_hid_init(), else NULL.
 */
const hid_sink_t *usb_hid_get_sink(void);

/**
 * @brief Assert or release USB soft-disconnect (DCTL.SFTDISCON) on OTG_HS.
 *
 * When VBUS sensing is active (CONFIG_USB_DEVICE_VBUS_SENSE_GPIO >= 0), connection
 * state is owned exclusively by usb_vbus_ctrl and manual calls return ESP_ERR_INVALID_STATE.
 * After stage-2 FORCE_OFF uninstalls TinyUSB, USB cannot be reconnected until system reboot.
 */
esp_err_t usb_hid_set_connected(bool connected);

#ifdef __cplusplus
}
#endif
