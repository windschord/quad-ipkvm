#include "iso_worker_logic.h"

#include <string.h>

static void wake_worker(const iso_worker_logic_t *w) {
    if (w && w->cfg.os.wake_worker) {
        w->cfg.os.wake_worker(w->cfg.os.ctx);
    }
}

static bool has_pending_core_work(const iso_worker_logic_t *w) {
    return (iso_core_queue_len(&w->core) > 0u) ||
           (iso_core_index_pending(&w->core) > 0u) ||
           w->core.cur_active ||
           atomic_load_explicit(&w->reinit_pending, memory_order_acquire) ||
           atomic_load_explicit(&w->reprobe_pending, memory_order_acquire);
}

static void on_media_force_eject_cb(void *ctx) {
    iso_worker_logic_t *w = (iso_worker_logic_t *)ctx;
    if (w && w->cfg.bdev && w->cfg.bdev->epoch_bump) {
        w->cfg.bdev->epoch_bump(w->cfg.bdev->ctx);
    }
}

static void on_core_view_update_cb(void *ctx, const iso_view_t *v) {
    iso_worker_logic_t *w = (iso_worker_logic_t *)ctx;
    if (!w || !v) {
        return;
    }

    iso_worker_table_slot_t next_slots[ISO_MAX_IMAGES];
    size_t next_n = 0u;
    memset(next_slots, 0, sizeof(next_slots));

    const size_t n = (v->count > ISO_MAX_IMAGES) ? ISO_MAX_IMAGES : v->count;
    for (size_t i = 0u; i < n; i++) {
        const iso_view_image_t *img = &v->images[i];
        uint32_t id_u32 = 0u;
        if (img->state == ISO_STATE_READY && img->table != NULL &&
            iso_worker_logic_id_to_u32(img->id, &id_u32)) {
            memcpy(next_slots[next_n].id, img->id, ISO_ID_LEN + 1u);
            next_slots[next_n].id_u32 = id_u32;
            next_slots[next_n].size_bytes = img->size_bytes;
            iso_extent_table_ref(img->table);
            next_slots[next_n].table = img->table;
            next_n++;
        }
    }

    iso_extent_table_t *to_unref[ISO_MAX_IMAGES];
    size_t unref_n = 0u;

    w->cfg.os.state_lock(w->cfg.os.ctx);
    for (size_t i = 0u; i < w->pub_table_n; i++) {
        if (w->pub_tables[i].table != NULL) {
            to_unref[unref_n++] = w->pub_tables[i].table;
        }
    }
    memset(w->pub_tables, 0, sizeof(w->pub_tables));
    if (next_n > 0u) {
        memcpy(w->pub_tables, next_slots, next_n * sizeof(next_slots[0]));
    }
    w->pub_table_n = next_n;

    w->pub_view = *v;
    w->pub_view.count = n;
    for (size_t i = 0u; i < ISO_MAX_IMAGES; i++) {
        w->pub_view.images[i].table = NULL;
    }
    if (w->sd_override_non_ready) {
        w->pub_view.sd_state = w->sd_override_val;
    }
    w->pub_consumed_since = w->core.up.consumed_since;

    if (w->cfg.os.on_view_published) {
        w->cfg.os.on_view_published(w->cfg.os.ctx, &w->pub_view, w->pub_consumed_since);
    }
    w->cfg.os.state_unlock(w->cfg.os.ctx);

    for (size_t i = 0u; i < unref_n; i++) {
        (void)iso_extent_table_unref(to_unref[i]);
    }
}

static bool on_core_is_mounted_cb(void *ctx, const char *id) {
    iso_worker_logic_t *w = (iso_worker_logic_t *)ctx;
    uint32_t target_id = 0u;
    if (!w || !iso_worker_logic_id_to_u32(id, &target_id)) {
        return false;
    }
    w->cfg.os.state_lock(w->cfg.os.ctx);
    uint32_t cur_id = 0u;
    const bool loaded = iso_media_get_id(&w->media, &cur_id);
    w->cfg.os.state_unlock(w->cfg.os.ctx);
    return loaded && (cur_id == target_id);
}

static int deinit_fail_remount_cb(void *ctx) {
    (void)ctx;
    return ISO_ERR_NO_CARD;
}

bool iso_worker_logic_id_to_u32(const char *id, uint32_t *out) {
    if (!iso_id_valid(id)) {
        return false;
    }
    uint32_t v = 0u;
    for (size_t i = 0u; i < ISO_ID_LEN; i++) {
        char c = id[i];
        uint32_t d = (c >= 'a') ? (uint32_t)(c - 'a' + 10) : (uint32_t)(c - '0');
        v = (v << 4) | d;
    }
    if (out) {
        *out = v;
    }
    return true;
}

int iso_worker_logic_init(iso_worker_logic_t *w, const iso_worker_logic_cfg_t *cfg) {
    if (!w || !cfg || !cfg->fs || !cfg->os.state_lock || !cfg->os.state_unlock ||
        !cfg->os.op_lock || !cfg->os.op_trylock || !cfg->os.op_unlock) {
        return ISO_ERR_INVALID;
    }
    memset(w, 0, sizeof(*w));
    w->cfg = *cfg;
    if (w->cfg.step_budget_bytes == 0u) {
        w->cfg.step_budget_bytes = (uint32_t)ISO_WORKER_BUDGET_KIB * 1024u;
    }
    atomic_init(&w->reinit_pending, false);
    atomic_init(&w->reprobe_pending, false);

    iso_catalog_init(&w->cat, cfg->fs, cfg->rnd);
    iso_media_init(&w->media, on_media_force_eject_cb, w);

    const iso_core_cfg_t core_cfg = {
        .cat = &w->cat,
        .bdev = cfg->bdev,
        .clock = cfg->clock,
        .sha = cfg->sha,
        .sha_state = cfg->sha_state,
        .io_buf = cfg->io_buf,
        .io_buf_size = cfg->io_buf_size,
        .alloc = cfg->alloc,
        .sd = cfg->sd,
        .chunk_bytes = cfg->chunk_bytes,
        .sync_bytes = cfg->sync_bytes,
        .hooks = {
            .ctx = w,
            .view_update = on_core_view_update_cb,
            .is_mounted = on_core_is_mounted_cb,
        },
    };
    int rc = iso_core_init(&w->core, &core_cfg);
    if (rc != ISO_OK) {
        memset(w, 0, sizeof(*w));
        return rc;
    }
    w->initialized = true;
    return ISO_OK;
}

void iso_worker_logic_deinit(iso_worker_logic_t *w) {
    if (!w || !w->initialized) {
        return;
    }
    w->cfg.os.op_lock(w->cfg.os.ctx);
    (void)iso_core_sd_reinit(&w->core, deinit_fail_remount_cb, NULL);
    w->cfg.os.op_unlock(w->cfg.os.ctx);

    iso_extent_table_t *to_unref[ISO_MAX_IMAGES];
    size_t unref_n = 0u;
    w->cfg.os.state_lock(w->cfg.os.ctx);
    for (size_t i = 0u; i < w->pub_table_n; i++) {
        if (w->pub_tables[i].table != NULL) {
            to_unref[unref_n++] = w->pub_tables[i].table;
        }
    }
    memset(w->pub_tables, 0, sizeof(w->pub_tables));
    w->pub_table_n = 0u;
    w->cfg.os.state_unlock(w->cfg.os.ctx);

    for (size_t i = 0u; i < unref_n; i++) {
        (void)iso_extent_table_unref(to_unref[i]);
    }
    w->initialized = false;
}

int iso_worker_logic_boot(iso_worker_logic_t *w) {
    if (!w || !w->initialized) {
        return ISO_ERR_INVALID;
    }
    int rc = ISO_OK;
    bool dirs_failed = false;
    w->cfg.os.op_lock(w->cfg.os.ctx);
    const bool ready = (w->cfg.sd != NULL) && (w->cfg.sd->state(w->cfg.sd->ctx) == ISO_SD_READY);
    if (ready) {
        rc = iso_catalog_ensure_dirs(&w->cat);
        if (rc == ISO_OK) {
            rc = iso_core_boot(&w->core);
        } else {
            dirs_failed = true;
            w->cfg.os.state_lock(w->cfg.os.ctx);
            w->sd_override_non_ready = true;
            w->sd_override_val = ISO_SD_ERROR;
            w->cfg.os.state_unlock(w->cfg.os.ctx);
            iso_core_publish(&w->core);
        }
    } else {
        iso_core_publish(&w->core);
    }
    const bool wake = has_pending_core_work(w);
    w->cfg.os.op_unlock(w->cfg.os.ctx);

    if (dirs_failed && w->cfg.os.set_hw_sd_state) {
        w->cfg.os.state_lock(w->cfg.os.ctx);
        w->cfg.os.set_hw_sd_state(w->cfg.os.ctx, ISO_SD_ERROR);
        w->cfg.os.state_unlock(w->cfg.os.ctx);
    }

    if (wake) {
        wake_worker(w);
    }
    return rc;
}

void iso_worker_logic_notify_sd_fault(iso_worker_logic_t *w, iso_sd_state_t fault_state) {
    if (!w || !w->initialized) {
        return;
    }
    const iso_sd_state_t non_ready = (fault_state == ISO_SD_READY) ? ISO_SD_NO_CARD : fault_state;

    /* §3.4 Step (1): keep sd.state non-READY, force_eject, epoch_bump, and reinit_pending
     * consistent inside one iso_state_mutex critical section. */
    w->cfg.os.state_lock(w->cfg.os.ctx);
    if (w->cfg.os.set_hw_sd_state) {
        w->cfg.os.set_hw_sd_state(w->cfg.os.ctx, non_ready);
    }
    w->sd_override_non_ready = true;
    w->sd_override_val = non_ready;
    w->pub_view.sd_state = non_ready;
    if (w->cfg.os.on_view_published) {
        w->cfg.os.on_view_published(w->cfg.os.ctx, &w->pub_view, w->pub_consumed_since);
    }
    iso_media_force_eject(&w->media);
    atomic_store_explicit(&w->reinit_pending, true, memory_order_release);
    w->cfg.os.state_unlock(w->cfg.os.ctx);

    wake_worker(w);
}

void iso_worker_logic_request_reprobe(iso_worker_logic_t *w) {
    if (!w || !w->initialized) {
        return;
    }
    atomic_store_explicit(&w->reprobe_pending, true, memory_order_release);
    wake_worker(w);
}

int iso_worker_logic_run_once(iso_worker_logic_t *w,
                              int (*remount)(void *ctx),
                              void *remount_ctx,
                              bool *out_busy,
                              bool *out_waiting) {
    if (out_busy) {
        *out_busy = false;
    }
    if (out_waiting) {
        *out_waiting = false;
    }
    if (!w || !w->initialized) {
        return ISO_ERR_INVALID;
    }

    bool do_reinit = atomic_exchange_explicit(&w->reinit_pending, false, memory_order_acq_rel);
    bool do_reprobe = atomic_exchange_explicit(&w->reprobe_pending, false, memory_order_acq_rel);
    if (!do_reinit && do_reprobe) {
        w->cfg.os.state_lock(w->cfg.os.ctx);
        const bool already_ready =
            (w->cfg.sd != NULL) && (w->cfg.sd->state(w->cfg.sd->ctx) == ISO_SD_READY) &&
            !w->sd_override_non_ready;
        w->cfg.os.state_unlock(w->cfg.os.ctx);
        if (!already_ready) {
            do_reinit = true;
        }
    }

    if (do_reinit) {
        w->cfg.os.op_lock(w->cfg.os.ctx);
        int rc = iso_core_sd_reinit(&w->core, remount, remount_ctx);
        w->cfg.os.state_lock(w->cfg.os.ctx);
        const bool fault_raced = atomic_load_explicit(&w->reinit_pending, memory_order_acquire);
        if (!fault_raced) {
            w->sd_override_non_ready = false;
        }
        w->cfg.os.state_unlock(w->cfg.os.ctx);
        if (!fault_raced) {
            iso_core_publish(&w->core);
        }
        const bool more = has_pending_core_work(w);
        w->cfg.os.op_unlock(w->cfg.os.ctx);
        if (out_busy) {
            *out_busy = more;
        }
        return rc;
    }

    w->cfg.os.op_lock(w->cfg.os.ctx);
    iso_job_t *job = iso_core_pick(&w->core);
    if (!job) {
        const bool more = has_pending_core_work(w);
        w->cfg.os.op_unlock(w->cfg.os.ctx);
        if (out_busy) {
            *out_busy = more;
        }
        return ISO_OK;
    }

    const iso_job_kind_t kind = job->kind;
    char job_id[ISO_ID_LEN + 1u];
    memcpy(job_id, job->id, sizeof(job_id));

    const iso_step_result_t r = iso_job_step(&w->core, job, w->cfg.step_budget_bytes);
    if (r.kind == ISO_STEP_CONTINUE) {
        const bool waiting = iso_job_waiting(job);
        w->cfg.os.op_unlock(w->cfg.os.ctx);
        if (out_busy) {
            *out_busy = true;
        }
        if (out_waiting) {
            *out_waiting = waiting;
        }
        return ISO_OK;
    }

    const bool more = has_pending_core_work(w);
    w->cfg.os.op_unlock(w->cfg.os.ctx);

    if (r.kind == ISO_STEP_ERROR && w->cfg.os.on_job_error) {
        w->cfg.os.on_job_error(w->cfg.os.ctx, kind, job_id, r.err);
    }
    if (out_busy) {
        *out_busy = more;
    }
    return (r.kind == ISO_STEP_ERROR) ? r.err : ISO_OK;
}

void iso_worker_logic_tick(iso_worker_logic_t *w, uint64_t now_ms) {
    if (!w || !w->initialized) {
        return;
    }
    w->cfg.os.op_lock(w->cfg.os.ctx);
    iso_upload_tick(&w->core, now_ms);
    const bool wake = has_pending_core_work(w);
    w->cfg.os.op_unlock(w->cfg.os.ctx);

    if (wake) {
        wake_worker(w);
    }
}

bool iso_worker_logic_op_trylock(iso_worker_logic_t *w, uint32_t wait_ms) {
    if (!w || !w->initialized) {
        return false;
    }
    return w->cfg.os.op_trylock(w->cfg.os.ctx, wait_ms);
}

void iso_worker_logic_op_unlock(iso_worker_logic_t *w) {
    if (!w || !w->initialized) {
        return;
    }
    const bool wake = has_pending_core_work(w);
    w->cfg.os.op_unlock(w->cfg.os.ctx);
    if (wake) {
        wake_worker(w);
    }
}

void iso_worker_logic_state_lock(iso_worker_logic_t *w) {
    if (w && w->initialized) {
        w->cfg.os.state_lock(w->cfg.os.ctx);
    }
}

void iso_worker_logic_state_unlock(iso_worker_logic_t *w) {
    if (w && w->initialized) {
        w->cfg.os.state_unlock(w->cfg.os.ctx);
    }
}

const iso_view_t *iso_worker_logic_view_locked(const iso_worker_logic_t *w,
                                               uint64_t *out_consumed_since) {
    if (!w || !w->initialized) {
        if (out_consumed_since) {
            *out_consumed_since = 0u;
        }
        return NULL;
    }
    if (out_consumed_since) {
        *out_consumed_since = w->pub_consumed_since;
    }
    return &w->pub_view;
}

int iso_worker_logic_lookup_size_locked(const iso_worker_logic_t *w,
                                        uint32_t media_id,
                                        uint64_t *out_bytes) {
    if (!w || !w->initialized || media_id == 0u || !out_bytes) {
        return ISO_ERR_INVALID;
    }
    for (size_t i = 0u; i < w->pub_table_n; i++) {
        if (w->pub_tables[i].id_u32 == media_id && w->pub_tables[i].table != NULL) {
            *out_bytes = w->pub_tables[i].size_bytes;
            return ISO_OK;
        }
    }
    return ISO_ERR_NOT_FOUND;
}

iso_extent_table_t *iso_worker_logic_acquire_table_locked(const iso_worker_logic_t *w,
                                                          uint32_t media_id) {
    if (!w || !w->initialized || media_id == 0u) {
        return NULL;
    }
    for (size_t i = 0u; i < w->pub_table_n; i++) {
        if (w->pub_tables[i].id_u32 == media_id && w->pub_tables[i].table != NULL) {
            iso_extent_table_ref(w->pub_tables[i].table);
            return w->pub_tables[i].table;
        }
    }
    return NULL;
}

iso_core_t *iso_worker_logic_core(iso_worker_logic_t *w) {
    return (w && w->initialized) ? &w->core : NULL;
}

iso_media_t *iso_worker_logic_media(iso_worker_logic_t *w) {
    return (w && w->initialized) ? &w->media : NULL;
}
