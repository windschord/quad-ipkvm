#ifndef ISO_USB_MSC_GLUE_H
#define ISO_USB_MSC_GLUE_H

/**
 * @file iso_usb_msc_glue.h
 * @brief Pure-C bridge between msc_bot, mmc_scsi, iso_extent, and iso_blockdev_t.
 *
 * Host-testable with ASan/UBSan and free of ESP-IDF/FreeRTOS/TinyUSB types.
 */

#include <stdbool.h>
#include <stdint.h>

#include "iso_extent.h"
#include "iso_media.h"
#include "iso_seams.h"
#include "mmc_scsi.h"
#include "msc_bot.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Execute one command descriptor emitted by msc_bot against mmc_scsi.
 *
 * Locking contract (see mmc_scsi.h): caller MUST hold iso_state_mutex around
 * MSC_BOT_CMD_EXECUTE, and MUST NOT hold iso_state_mutex around
 * MSC_BOT_CMD_READ_BLOCKS (blocking SD I/O).
 */
void iso_usb_msc_exec_cmd(mmc_scsi_t *scsi, const msc_bot_cmd_t *cmd, msc_bot_cmd_result_t *out_res);

/**
 * @brief Optional external table resolver (e.g. iso_worker) invoked under shared iso_state_mutex.
 *
 * acquire_table_locked MUST return a table with an incremented reference count (released later via
 * iso_extent_table_unref() inside mmc_scsi_backend_t.release()).
 */
typedef struct {
    void *ctx;
    int (*lookup_size_locked)(void *ctx, uint32_t media_id, uint64_t *out_bytes);
    iso_extent_table_t *(*acquire_table_locked)(void *ctx, uint32_t media_id);
} iso_usb_msc_table_resolver_t;

/**
 * @brief Backing state for mmc_scsi_backend_t built over iso_extent_table_t + iso_blockdev_t.
 */
typedef struct {
    const iso_blockdev_t *bdev;
    uint32_t media_id;
    iso_extent_table_t *table;
    iso_usb_msc_table_resolver_t resolver;
    void *lock_ctx;
    void (*lock)(void *ctx);
    void (*unlock)(void *ctx);
} iso_usb_msc_extent_backend_t;

/** @brief Initialize extent backend with optional lightweight state lock. */
void iso_usb_msc_extent_backend_init(iso_usb_msc_extent_backend_t *be,
                                     const iso_blockdev_t *bdev,
                                     void (*lock)(void *ctx),
                                     void (*unlock)(void *ctx),
                                     void *lock_ctx);

/** @brief Install or clear an external locked table resolver (e.g. iso_worker_logic). */
void iso_usb_msc_extent_backend_set_resolver(iso_usb_msc_extent_backend_t *be,
                                             const iso_usb_msc_table_resolver_t *resolver);

/** @brief Check whether @p table's snapshot epoch matches the current blockdev epoch. */
bool iso_usb_msc_table_epoch_matches(const iso_blockdev_t *bdev, const iso_extent_table_t *table);

/** @brief Clear prevent_removal on physical/soft USB disconnect per ISO_MOUNT_DESIGN §5.5. */
void iso_usb_msc_on_disconnect_media(iso_media_t *media);

/** @brief Replace or clear the underlying raw block device handle. */
void iso_usb_msc_extent_backend_set_blockdev(iso_usb_msc_extent_backend_t *be,
                                             const iso_blockdev_t *bdev);

/**
 * @brief Install (or replace) the active extent table for @p media_id.
 *
 * Takes its own reference on @p table (if non-NULL) and releases the previous table.
 */
void iso_usb_msc_extent_backend_set_image(iso_usb_msc_extent_backend_t *be,
                                          uint32_t media_id,
                                          iso_extent_table_t *table);

/** @brief Clear the active extent table (equivalent to set_image(..., 0, NULL)). */
void iso_usb_msc_extent_backend_clear(iso_usb_msc_extent_backend_t *be);

/**
 * @brief Synchronize backend extent retention with media state (clears backend ref when not loaded).
 *
 * Call under iso_state_mutex after SCSI EXECUTE commands (e.g., target START STOP UNIT eject),
 * on command_done, and on status reads so an ejected extent table is unreferenced immediately.
 */
void iso_usb_msc_sync_media_backend(const iso_media_t *media,
                                    iso_usb_msc_extent_backend_t *be);

/** @brief Construct an mmc_scsi_backend_t bound to @p be. */
mmc_scsi_backend_t iso_usb_msc_extent_backend_as_scsi(iso_usb_msc_extent_backend_t *be);

#ifdef __cplusplus
}
#endif

#endif /* ISO_USB_MSC_GLUE_H */
