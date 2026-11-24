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
#include "iso_usb_msc_glue.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const iso_blockdev_t *bdev;
    /* Optional shared iso_media + iso_state_mutex (e.g. iso_worker) so HTTP API and MSC share
     * one authoritative owner. Both state_lock and state_unlock are required when non-NULL. */
    iso_media_t *shared_media;
    void *state_lock_ctx;
    void (*state_lock)(void *ctx);
    void (*state_unlock)(void *ctx);
    iso_usb_msc_table_resolver_t resolver;
} iso_usb_msc_config_t;

/**
 * @brief Initialize MSC BOT/SCSI/media state, DMA ping-pong buffers, and iso_msc_task.
 *
 * @param bdev Optional raw block device for reading ISO extents; may be NULL (enumerates EMPTY).
 */
esp_err_t iso_usb_msc_init(const iso_blockdev_t *bdev);

/** @brief Extended init allowing shared iso_media, external state lock, and table resolver. */
esp_err_t iso_usb_msc_init_ex(const iso_usb_msc_config_t *cfg);

/** @brief Update or clear the raw block device handle used for extent reads. */
void iso_usb_msc_set_blockdev(const iso_blockdev_t *bdev);

/** @brief Notify MSC media state of USB detach / soft-disconnect so prevent_removal clears. */
void iso_usb_msc_notify_disconnect(void);

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
