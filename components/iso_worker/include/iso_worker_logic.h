#ifndef ISO_WORKER_LOGIC_H
#define ISO_WORKER_LOGIC_H

/*
 * iso_worker_logic: Pure-C host-testable coordinator gluing iso_core, iso_catalog,
 * iso_upload, iso_media, and iso_seams (docs/ISO_MOUNT_DESIGN.md §3.4, §4.1, §4.2).
 *
 * Locking discipline (ISO_MOUNT_DESIGN.md §4.1):
 *   - iso_op_mutex (Level 1): guards all iso_core_* / iso_upload_* / iso_catalog_* / remount calls.
 *   - iso_state_mutex (Level 2): short memory-only lock guarding pub_view, pub_tables, iso_media,
 *     and sd_override_non_ready. Never acquire op_mutex or perform SD I/O under state_mutex.
 *   - op_unlock() checks for pending queued/index work before releasing op_mutex and invokes
 *     wake_worker() after unlocking.
 */

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "iso_catalog.h"
#include "iso_core.h"
#include "iso_extent.h"
#include "iso_media.h"
#include "iso_seams.h"
#include "iso_upload.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    void *ctx;
    void (*state_lock)(void *ctx);
    void (*state_unlock)(void *ctx);
    void (*op_lock)(void *ctx);
    bool (*op_trylock)(void *ctx, uint32_t wait_ms);
    void (*op_unlock)(void *ctx);
    /* Invoked with NO locks held when background work is pending; may be NULL. */
    void (*wake_worker)(void *ctx);
    /* Invoked inside state_lock in Step (1) of SD fault handling; may be NULL. */
    void (*set_hw_sd_state)(void *ctx, iso_sd_state_t st);
    /* Invoked inside state_lock whenever pub_view changes; may be NULL. */
    void (*on_view_published)(void *ctx, const iso_view_t *view, uint64_t consumed_since);
    /* Invoked with NO locks held when iso_job_step returns ISO_STEP_ERROR; may be NULL. */
    void (*on_job_error)(void *ctx, iso_job_kind_t kind, const char *id, int err);
} iso_worker_os_ops_t;

typedef struct {
    const iso_fs_t *fs;
    const iso_blockdev_t *bdev;
    const iso_clock_t *clock;
    const iso_random_t *rnd;
    const iso_sha256_t *sha;
    void *sha_state;
    uint8_t *io_buf;
    size_t io_buf_size;
    const iso_allocator_t *alloc;
    const iso_sd_status_t *sd;
    uint32_t chunk_bytes;       /* 0 = iso_upload default */
    uint32_t sync_bytes;        /* 0 = iso_upload default */
    uint32_t step_budget_bytes; /* 0 = ISO_WORKER_BUDGET_KIB * 1024 */
    iso_worker_os_ops_t os;
} iso_worker_logic_cfg_t;

typedef struct {
    char id[ISO_ID_LEN + 1];
    uint32_t id_u32;
    uint64_t size_bytes;
    iso_extent_table_t *table;
} iso_worker_table_slot_t;

typedef struct {
    iso_worker_logic_cfg_t cfg;
    bool initialized;
    iso_catalog_t cat;
    iso_media_t media;
    iso_core_t core;

    /* Guarded by cfg.os.state_lock / state_unlock: */
    iso_view_t pub_view;
    uint64_t pub_consumed_since;
    iso_worker_table_slot_t pub_tables[ISO_MAX_IMAGES];
    size_t pub_table_n;
    bool sd_override_non_ready;
    iso_sd_state_t sd_override_val;

    /* Lock-free cross-task wake/fault coordination: */
    _Atomic bool reinit_pending;
    _Atomic bool reprobe_pending;
} iso_worker_logic_t;

/* Convert canonical 8-hex-digit image ID string to uint32_t (matches iso_api_id_to_u32). */
bool iso_worker_logic_id_to_u32(const char *id, uint32_t *out);

int iso_worker_logic_init(iso_worker_logic_t *w, const iso_worker_logic_cfg_t *cfg);
void iso_worker_logic_deinit(iso_worker_logic_t *w);

/* Execute boot scan/publish under op_lock and wake worker if queued/index jobs exist. */
int iso_worker_logic_boot(iso_worker_logic_t *w);

/* Step (1) of docs/ISO_MOUNT_DESIGN.md §3.4: sets non-READY view/state, force-ejects media,
 * and bumps blockdev epoch inside ONE state_lock section, then queues Steps (2)-(7) on worker. */
void iso_worker_logic_notify_sd_fault(iso_worker_logic_t *w, iso_sd_state_t fault_state);

/* Notify worker to probe/remount SD when card is not READY. */
void iso_worker_logic_request_reprobe(iso_worker_logic_t *w);

/* Run one reinit/reprobe pass or one iso_job_step() under op_lock.
 * Sets *out_busy=true if more work remains immediately; *out_waiting=true if waiting on MSC refs. */
int iso_worker_logic_run_once(iso_worker_logic_t *w,
                              int (*remount)(void *ctx),
                              void *remount_ctx,
                              bool *out_busy,
                              bool *out_waiting);

/* Periodic 1s tick (drives iso_upload_tick and wakes worker if IDLE_CLOSE queued). */
void iso_worker_logic_tick(iso_worker_logic_t *w, uint64_t now_ms);

/* Locking wrappers for iso_api / HTTP / MSC consumers. */
bool iso_worker_logic_op_trylock(iso_worker_logic_t *w, uint32_t wait_ms);
void iso_worker_logic_op_unlock(iso_worker_logic_t *w);
void iso_worker_logic_state_lock(iso_worker_logic_t *w);
void iso_worker_logic_state_unlock(iso_worker_logic_t *w);

/* Must be called while holding state_lock. */
const iso_view_t *iso_worker_logic_view_locked(const iso_worker_logic_t *w,
                                               uint64_t *out_consumed_since);
int iso_worker_logic_lookup_size_locked(const iso_worker_logic_t *w,
                                        uint32_t media_id,
                                        uint64_t *out_bytes);
iso_extent_table_t *iso_worker_logic_acquire_table_locked(const iso_worker_logic_t *w,
                                                          uint32_t media_id);

iso_core_t *iso_worker_logic_core(iso_worker_logic_t *w);
iso_media_t *iso_worker_logic_media(iso_worker_logic_t *w);

#ifdef __cplusplus
}
#endif

#endif /* ISO_WORKER_LOGIC_H */
