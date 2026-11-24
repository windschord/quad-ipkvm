#ifndef ISO_USB_MSC_H
#define ISO_USB_MSC_H

/**
 * @file iso_usb_msc.h
 * @brief ESP-IDF TinyUSB custom class driver and FreeRTOS worker for Virtual CD-ROM (MSC).
 *
 * Registers via TinyUSB's usbd_app_driver_get_cb() strong symbol when CONFIG_USB_MSC_ENABLE=y.
 * Must be initialized before usb_hid_init() so the linker includes this translation unit and
 * TinyUSB installs the custom driver alongside the built-in HID class driver.
 */

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "iso_extent.h"
#include "iso_media.h"
#include "iso_seams.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize MSC BOT/SCSI/media state, DMA ping-pong buffers, and iso_msc_task.
 *
 * @param bdev Optional raw block device for reading ISO extents; may be NULL (enumerates EMPTY).
 */
esp_err_t iso_usb_msc_init(const iso_blockdev_t *bdev);

/** @brief Update or clear the raw block device handle used for extent reads. */
void iso_usb_msc_set_blockdev(const iso_blockdev_t *bdev);

/** @brief Mount an ISO extent table for @p media_id on the virtual CD-ROM drive. */
esp_err_t iso_usb_msc_mount_image(uint32_t media_id, iso_extent_table_t *table, bool force);

/** @brief Eject the active ISO image from the virtual CD-ROM drive. */
esp_err_t iso_usb_msc_eject_image(bool force);

/** @brief Force-eject immediately on storage removal / SD loss (sets UA and aborts reads). */
void iso_usb_msc_force_eject(void);

/** @brief Snapshot current media state for API/status reporting. */
esp_err_t iso_usb_msc_get_status(iso_media_status_t *out);

/** @brief Return true once iso_usb_msc_init() has succeeded. */
bool iso_usb_msc_is_initialized(void);

#ifdef __cplusplus
}
#endif

#endif /* ISO_USB_MSC_H */
