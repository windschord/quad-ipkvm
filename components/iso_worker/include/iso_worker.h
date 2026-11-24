#ifndef ISO_WORKER_H
#define ISO_WORKER_H

/*
 * iso_worker: ESP-IDF / FreeRTOS coordinator owning iso_core, iso_media,
 * iso_catalog, iso_upload, and managed iso_sd recovery
 * (docs/ISO_MOUNT_DESIGN.md §3.4, §4.1, §4.2).
 */

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "iso_core.h"
#include "iso_extent.h"
#include "iso_media.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Invoked under iso_state_mutex whenever published ISO state changes (short, no I/O). */
typedef void (*iso_worker_view_cb_t)(void *ctx, const iso_view_t *view, uint64_t consumed_since);

esp_err_t iso_worker_init(void);
void iso_worker_deinit(void);
bool iso_worker_is_running(void);

void iso_worker_set_view_cb(iso_worker_view_cb_t cb, void *ctx);

void iso_worker_state_lock(void);
void iso_worker_state_unlock(void);
bool iso_worker_op_trylock(uint32_t wait_ms);
void iso_worker_op_unlock(void);

iso_core_t *iso_worker_core(void);
iso_media_t *iso_worker_media(void);

/* Must be called while holding iso_worker_state_lock().
 * iso_worker_acquire_table_locked() increments the extent-table refcount: release via
 * iso_extent_table_unref() after unlocking state_lock; returns NULL for media_id == 0 or unknown IDs. */
const iso_view_t *iso_worker_view_locked(uint64_t *out_consumed_since);
int iso_worker_lookup_size_locked(uint32_t media_id, uint64_t *out_bytes);
iso_extent_table_t *iso_worker_acquire_table_locked(uint32_t media_id);

#ifdef __cplusplus
}
#endif

#endif /* ISO_WORKER_H */
