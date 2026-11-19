#include "iso_core.h"

#include <string.h>

static uint64_t clock_now(const iso_core_t *c) { return c->cfg.clock->now_ms(c->cfg.clock->ctx); }

static iso_step_result_t sres(iso_step_t kind, int err) {
    iso_step_result_t r = {kind, err};
    return r;
}

/* ---- エクステントの表の持ち主(カタログの分の参照 1) ------------------------------------------ */

iso_extent_table_t *iso_core_table(const iso_core_t *c, const char *id) {
    for (size_t i = 0; i < c->table_n; i++) {
        if (strcmp(c->tables[i].id, id) == 0) return c->tables[i].table;
    }
    return NULL;
}

static void table_remove(iso_core_t *c, const char *id) {
    for (size_t i = 0; i < c->table_n; i++) {
        if (strcmp(c->tables[i].id, id) != 0) continue;
        (void)iso_extent_table_unref(c->tables[i].table);
        for (size_t k = i + 1; k < c->table_n; k++) c->tables[k - 1] = c->tables[k];
        c->table_n--;
        memset(&c->tables[c->table_n], 0, sizeof(c->tables[0]));
        return;
    }
}

/* 所有権を受け取る。同じ id の以前の表は放す。置き場がなければ、受け取った表を放す。 */
static void table_install(iso_core_t *c, const char *id, iso_extent_table_t *t) {
    table_remove(c, id);
    if (!t) return;
    if (c->table_n >= ISO_MAX_IMAGES) {
        (void)iso_extent_table_unref(t);
        return;
    }
    memcpy(c->tables[c->table_n].id, id, ISO_ID_LEN + 1);
    c->tables[c->table_n].table = t;
    c->table_n++;
}

static void table_clear_all(iso_core_t *c) {
    for (size_t i = 0; i < c->table_n; i++) {
        (void)iso_extent_table_unref(c->tables[i].table);
        memset(&c->tables[i], 0, sizeof(c->tables[i]));
    }
    c->table_n = 0;
}

static void on_table_ready(void *ctx, const char *id, iso_extent_table_t *t) { table_install((iso_core_t *)ctx, id, t); }

/* ---- ビュー --------------------------------------------------------------------------- */

static void publish_at(iso_core_t *c, uint64_t now) {
    iso_view_t *v = &c->view;
    const iso_catalog_t *cat = c->cfg.cat;
    memset(v, 0, sizeof(*v));
    v->sd_state = c->cfg.sd ? c->cfg.sd->state(c->cfg.sd->ctx) : ISO_SD_NO_CARD;
    v->free_known = c->up.free_known;
    v->total_bytes = c->up.total_bytes;
    v->free_bytes = c->up.free_bytes;
    v->hidden = cat->hidden;
    v->chunk_bytes = c->up.cfg.chunk_bytes;
    v->count = cat->count;
    for (size_t i = 0; i < cat->count; i++) {
        const iso_catalog_entry_t *e = &cat->images[i];
        iso_view_image_t *w = &v->images[i];
        memcpy(w->id, e->id, ISO_ID_LEN + 1);
        w->state = e->state;
        w->invalid_reason = e->invalid_reason;
        w->meta_valid = e->meta_valid;
        w->received = e->received;
        if (e->meta_valid) {
            w->name_len = e->meta.name_len;
            memcpy(w->name, e->meta.name, sizeof(w->name));
            w->size_bytes = e->meta.size_bytes;
        }
        if (e->state == ISO_STATE_READY) {
            w->table = iso_core_table(c, e->id);
            if (!w->table) w->state = ISO_STATE_FINALIZING; /* INDEX 待ち・中 */
            if (e->meta_valid && e->meta.sha_flag == ISO_SHA_COMPUTED) {
                w->has_sha = true;
                memcpy(w->sha256, e->meta.sha256, ISO_SHA256_LEN);
            }
        }
    }
    v->upload_active = c->up.slot_active;
    if (c->up.slot_active) memcpy(v->upload_id, c->up.slot_id, ISO_ID_LEN + 1);
    v->upload_state = iso_upload_activity(&c->up, now);
    c->pub_act = v->upload_state;
    if (c->cfg.hooks.view_update) c->cfg.hooks.view_update(c->cfg.hooks.ctx, v);
}

void iso_core_publish(iso_core_t *c) { publish_at(c, clock_now(c)); }
const iso_view_t *iso_core_view(const iso_core_t *c) { return &c->view; }

static void on_view_changed(void *ctx) { iso_core_publish((iso_core_t *)ctx); }

/* ---- 初期化 --------------------------------------------------------------------------- */

int iso_core_init(iso_core_t *c, const iso_core_cfg_t *cfg) {
    if (!c || !cfg) return ISO_ERR_INVALID;
    memset(c, 0, sizeof(*c));
    c->cfg = *cfg;
    iso_upload_cfg_t uc;
    memset(&uc, 0, sizeof(uc));
    uc.cat = cfg->cat;
    uc.bdev = cfg->bdev;
    uc.clock = cfg->clock;
    uc.sha = cfg->sha;
    uc.sha_state = cfg->sha_state;
    uc.io_buf = cfg->io_buf;
    uc.io_buf_size = cfg->io_buf_size;
    uc.alloc = cfg->alloc;
    uc.chunk_bytes = cfg->chunk_bytes;
    uc.sync_bytes = cfg->sync_bytes;
    uc.hooks.ctx = c;
    uc.hooks.view_changed = on_view_changed;
    uc.hooks.table_ready = on_table_ready;
    return iso_upload_init(&c->up, &uc);
}

iso_upload_t *iso_core_upload(iso_core_t *c) { return &c->up; }
size_t iso_core_queue_len(const iso_core_t *c) { return c->qn; }
size_t iso_core_index_pending(const iso_core_t *c) { return c->index_n; }

/* ---- キュー、INDEX の集合 --------------------------------------------------------------- */

static void queue_remove_at(iso_core_t *c, size_t i) {
    for (size_t k = i + 1; k < c->qn; k++) c->queue[k - 1] = c->queue[k];
    c->qn--;
    memset(&c->queue[c->qn], 0, sizeof(c->queue[0]));
}

static bool queue_has(const iso_core_t *c, iso_job_kind_t kind, const char *id) {
    for (size_t i = 0; i < c->qn; i++) {
        if (c->queue[i].kind == kind && strcmp(c->queue[i].id, id) == 0) return true;
    }
    return false;
}

/* 同じ id の、待っているジョブの数(kind が NONE なら全種類) */
static size_t queue_count(const iso_core_t *c, iso_job_kind_t kind, const char *id) {
    size_t n = 0;
    for (size_t i = 0; i < c->qn; i++) {
        if ((kind == ISO_JOB_NONE || c->queue[i].kind == kind) && strcmp(c->queue[i].id, id) == 0) n++;
    }
    return n;
}

/* 同じ id の、待っているジョブ(kind が NONE なら全種類)を取り除く */
static void queue_drop(iso_core_t *c, iso_job_kind_t kind, const char *id) {
    size_t i = 0;
    while (i < c->qn) {
        if ((kind == ISO_JOB_NONE || c->queue[i].kind == kind) && strcmp(c->queue[i].id, id) == 0) {
            queue_remove_at(c, i);
        } else {
            i++;
        }
    }
}

static void queue_push(iso_core_t *c, iso_job_kind_t kind, const char *id) {
    c->queue[c->qn].kind = kind;
    memcpy(c->queue[c->qn].id, id, ISO_ID_LEN + 1);
    c->qn++;
}

static bool cur_is(const iso_core_t *c, iso_job_kind_t kind, const char *id) {
    return c->cur_active && c->cur.kind == kind && strcmp(c->cur.id, id) == 0;
}

static bool index_has(const iso_core_t *c, const char *id) {
    for (size_t i = 0; i < c->index_n; i++) {
        if (strcmp(c->index_ids[i], id) == 0) return true;
    }
    return false;
}

static void index_drop(iso_core_t *c, const char *id) {
    for (size_t i = 0; i < c->index_n; i++) {
        if (strcmp(c->index_ids[i], id) != 0) continue;
        for (size_t k = i + 1; k < c->index_n; k++) memcpy(c->index_ids[k - 1], c->index_ids[k], ISO_ID_LEN + 1);
        c->index_n--;
        memset(c->index_ids[c->index_n], 0, ISO_ID_LEN + 1);
        return;
    }
}

static void index_add(iso_core_t *c, const char *id) {
    if (index_has(c, id) || c->index_n >= ISO_MAX_IMAGES) return;
    memcpy(c->index_ids[c->index_n], id, ISO_ID_LEN + 1);
    c->index_n++;
}

void iso_core_mark_unindexed(iso_core_t *c) {
    for (size_t i = 0; i < c->cfg.cat->count; i++) {
        const iso_catalog_entry_t *e = &c->cfg.cat->images[i];
        if (e->state != ISO_STATE_READY || iso_core_table(c, e->id) || cur_is(c, ISO_JOB_INDEX, e->id)) continue;
        index_add(c, e->id);
    }
}

/* ---- 投入 ---------------------------------------------------------------------------- */

int iso_core_submit_finish(iso_core_t *c, const char *id, uint64_t *out_received) {
    iso_catalog_entry_t *e = iso_catalog_find(c->cfg.cat, id);
    iso_state_t prev = e ? e->state : ISO_STATE_NONE;
    int rc = iso_upload_finish_begin(&c->up, id, out_received);
    if (rc == ISO_RES_ALREADY_FINALIZING) {
        if (queue_has(c, ISO_JOB_FINISH, id) || cur_is(c, ISO_JOB_FINISH, id)) return ISO_RES_JOINED;
        rc = ISO_RES_OK; /* ジョブがない finalizing(あってはならない): 積み直す */
        prev = ISO_STATE_NONE;
    }
    if (rc != ISO_RES_OK) return rc;
    if (c->qn - queue_count(c, ISO_JOB_IDLE_CLOSE, id) >= ISO_WORKER_QUEUE) { /* 待っている IDLE_CLOSE は、FINISH に置き換わる */
        if (prev != ISO_STATE_NONE) iso_upload_finish_undo(&c->up, e, prev);
        return ISO_ERR_BUSY;
    }
    queue_drop(c, ISO_JOB_IDLE_CLOSE, id);
    queue_push(c, ISO_JOB_FINISH, id); /* ビューは、iso_upload_finish_begin が更新した(状態が finalizing になった) */
    return ISO_RES_OK;
}

int iso_core_submit_delete(iso_core_t *c, const char *id) {
    iso_catalog_entry_t *e = iso_catalog_find(c->cfg.cat, id);
    if (!e || e->state == ISO_STATE_NONE) return ISO_RES_NOT_FOUND;
    if (queue_has(c, ISO_JOB_DELETE, id) || cur_is(c, ISO_JOB_DELETE, id)) return ISO_RES_JOINED;
    iso_trans_ctx_t tc = {.mounted = c->cfg.hooks.is_mounted && c->cfg.hooks.is_mounted(c->cfg.hooks.ctx, id)};
    iso_state_t next;
    if (iso_state_transition(e->state, ISO_EV_DELETE, &tc, &next) == ISO_TR_REJECT_MOUNTED) return ISO_RES_MOUNTED;
    if (c->qn - queue_count(c, ISO_JOB_NONE, id) >= ISO_WORKER_QUEUE) return ISO_ERR_BUSY; /* 待っているジョブは、DELETE に置き換わる */

    int rc = iso_catalog_apply(c->cfg.cat, e, ISO_EV_DELETE, &tc, NULL);
    if (rc != 0) return rc > 0 ? (int)ISO_RES_NOT_FOUND : rc;
    iso_upload_abort(&c->up, id); /* uploading / finalizing: ファイルを閉じ、SHA-256 を捨て、枠を空ける */
    queue_drop(c, ISO_JOB_NONE, id);
    index_drop(c, id);
    if (c->cur_active && strcmp(c->cur.id, id) == 0) c->cur.aborted = true;
    queue_push(c, ISO_JOB_DELETE, id);
    iso_core_publish(c);
    return ISO_RES_OK;
}

int iso_core_submit(iso_core_t *c, iso_job_kind_t kind, const char *id) {
    switch (kind) {
    case ISO_JOB_FINISH: return iso_core_submit_finish(c, id, NULL);
    case ISO_JOB_DELETE: return iso_core_submit_delete(c, id);
    case ISO_JOB_IDLE_CLOSE: {
        const iso_catalog_entry_t *e = iso_catalog_find(c->cfg.cat, id);
        if (!e || e->state == ISO_STATE_NONE) return ISO_RES_NOT_FOUND;
        for (size_t i = 0; i < c->qn; i++) {
            if (strcmp(c->queue[i].id, id) == 0) return ISO_RES_JOINED;
        }
        if (c->cur_active && strcmp(c->cur.id, id) == 0) return ISO_RES_JOINED;
        if (c->qn >= ISO_WORKER_QUEUE) return ISO_ERR_BUSY;
        queue_push(c, ISO_JOB_IDLE_CLOSE, id);
        return ISO_RES_OK;
    }
    case ISO_JOB_INDEX: {
        const iso_catalog_entry_t *e = iso_catalog_find(c->cfg.cat, id);
        if (!e || e->state == ISO_STATE_NONE) return ISO_RES_NOT_FOUND;
        if (e->state != ISO_STATE_READY) return ISO_RES_NOT_UPLOADING;
        if (index_has(c, id) || (c->cur_active && strcmp(c->cur.id, id) == 0)) return ISO_RES_JOINED;
        index_add(c, id);
        return ISO_RES_OK;
    }
    default: return ISO_ERR_INVALID;
    }
}

/* ---- 起動、SD の再初期化 ---------------------------------------------------------------- */

static void job_release(iso_core_t *c) {
    if (!c->cur_active) return;
    if (c->cur.kind == ISO_JOB_FINISH) iso_finish_job_release(&c->up, &c->cur.fin);
    if (c->cur.kind == ISO_JOB_INDEX) iso_index_job_release(&c->up, &c->cur.idx);
    c->cur_active = false;
    memset(&c->cur, 0, sizeof(c->cur));
}

/* ②③④: ジョブの全取り消し、エクステントの表(カタログの分の参照)、ファイル、アップロード */
static void reset_runtime(iso_core_t *c) {
    c->qn = 0;
    memset(c->queue, 0, sizeof(c->queue));
    c->index_n = 0;
    memset(c->index_ids, 0, sizeof(c->index_ids));
    job_release(c);
    table_clear_all(c);
    iso_upload_reinit(&c->up);
}

/* ⑥ の後: deleting の DELETE を積み、ready を INDEX の集合へ */
static void schedule_after_scan(iso_core_t *c) {
    for (size_t i = 0; i < c->cfg.cat->count; i++) {
        const iso_catalog_entry_t *e = &c->cfg.cat->images[i];
        if (e->state == ISO_STATE_DELETING && c->qn < ISO_WORKER_QUEUE) queue_push(c, ISO_JOB_DELETE, e->id);
    }
    iso_core_mark_unindexed(c);
}

static int rebuild(iso_core_t *c) {
    int rc = iso_catalog_scan(c->cfg.cat);
    if (rc < 0) {
        c->cfg.cat->count = 0;
        c->cfg.cat->hidden = 0;
        iso_core_publish(c);
        return rc;
    }
    (void)iso_upload_refresh_free(&c->up);
    schedule_after_scan(c);
    iso_core_publish(c);
    return ISO_OK;
}

int iso_core_boot(iso_core_t *c) {
    reset_runtime(c);
    return rebuild(c);
}

int iso_core_sd_reinit(iso_core_t *c, int (*remount)(void *ctx), void *remount_ctx) {
    reset_runtime(c);
    if (remount) {
        int rc = remount(remount_ctx);
        if (rc < 0) {
            c->cfg.cat->count = 0;
            c->cfg.cat->hidden = 0;
            iso_core_publish(c);
            return rc;
        }
    }
    return rebuild(c);
}

/* ---- ワーカー ------------------------------------------------------------------------ */

static size_t queue_find_kind(const iso_core_t *c, iso_job_kind_t kind) {
    for (size_t i = 0; i < c->qn; i++) {
        if (c->queue[i].kind == kind) return i;
    }
    return c->qn;
}

iso_job_t *iso_core_pick(iso_core_t *c) {
    if (c->cur_active) return &c->cur;
    static const iso_job_kind_t order[] = {ISO_JOB_DELETE, ISO_JOB_FINISH, ISO_JOB_IDLE_CLOSE};
    for (size_t k = 0; k < sizeof(order) / sizeof(order[0]); k++) {
        size_t i = queue_find_kind(c, order[k]);
        if (i >= c->qn) continue;
        memset(&c->cur, 0, sizeof(c->cur));
        c->cur.kind = order[k];
        memcpy(c->cur.id, c->queue[i].id, ISO_ID_LEN + 1);
        queue_remove_at(c, i);
        c->cur_active = true;
        if (order[k] == ISO_JOB_DELETE) (void)iso_delete_job_init(&c->cur.del.dj, c->cur.id);
        if (order[k] == ISO_JOB_FINISH) (void)iso_finish_job_init(&c->cur.fin, c->cur.id);
        return &c->cur;
    }
    while (c->index_n > 0) {
        char id[ISO_ID_LEN + 1];
        memcpy(id, c->index_ids[0], sizeof(id));
        index_drop(c, id);
        const iso_catalog_entry_t *e = iso_catalog_find(c->cfg.cat, id);
        if (!e || e->state != ISO_STATE_READY) continue; /* もう ready でない(削除された、invalid になった) */
        memset(&c->cur, 0, sizeof(c->cur));
        c->cur.kind = ISO_JOB_INDEX;
        memcpy(c->cur.id, id, sizeof(id));
        (void)iso_index_job_init(&c->cur.idx, id);
        c->cur_active = true;
        return &c->cur;
    }
    return NULL;
}

bool iso_job_waiting(const iso_job_t *job) { return job && job->waiting; }

static iso_step_result_t end_job(iso_core_t *c, iso_step_result_t r) {
    job_release(c);
    iso_core_publish(c);
    return r;
}

static iso_step_result_t step_index(iso_core_t *c, iso_job_t *job) {
    iso_upload_t *u = &c->up;
    iso_step_result_t r = iso_index_step(u, &job->idx);
    if (r.kind == ISO_STEP_CONTINUE) return r;
    iso_catalog_entry_t *e = iso_catalog_find(c->cfg.cat, job->id);
    if (!e) return end_job(c, sres(ISO_STEP_DONE, ISO_OK));
    switch (job->idx.outcome) {
    case ISO_OUTCOME_OK: {
        iso_extent_table_t *t = job->idx.table;
        job->idx.table = NULL;
        table_install(c, job->id, t); /* ready のまま(INDEX_OK は、状態も meta も変えない) */
        return end_job(c, sres(ISO_STEP_DONE, ISO_OK));
    }
    case ISO_OUTCOME_INVALID: {
        iso_apply_arg_t arg = {.invalid_reason = job->idx.reason};
        int rc = iso_catalog_apply(c->cfg.cat, e, ISO_EV_INDEX_FAIL, NULL, &arg);
        if (rc != 0 && e->state == ISO_STATE_READY) { /* meta を書けなかった: メモリ上は invalid(次の起動で、もう一度判定される) */
            e->state = ISO_STATE_INVALID;
            e->invalid_reason = job->idx.reason;
        }
        table_remove(c, job->id);
        return end_job(c, sres(rc < 0 ? ISO_STEP_ERROR : ISO_STEP_DONE, rc < 0 ? rc : ISO_OK));
    }
    default: /* IOERR: ready のまま、表なし(SD の再初期化の後に、もう一度行う) */
        return end_job(c, sres(ISO_STEP_ERROR, job->idx.err)); /* ready のまま、表なし(INDEX_IOERR は、状態も meta も変えない) */
    }
}

static iso_step_result_t step_delete(iso_core_t *c, iso_job_t *job) {
    if (job->del.phase == 0) {
        iso_extent_table_t *t = iso_core_table(c, job->id);
        if (t && iso_extent_table_refs(t) > 1) {
            uint64_t now = clock_now(c);
            if (!job->del.wait_started) {
                job->del.wait_started = true;
                job->del.wait_start_ms = now;
            }
            if (now < job->del.wait_start_ms) job->del.wait_start_ms = now; /* 巻き戻り: 経過 0 */
            if (now - job->del.wait_start_ms >= (uint64_t)ISO_DELETE_REF_WAIT_S * 1000u) {
                return end_job(c, sres(ISO_STEP_ERROR, ISO_ERR_BUSY)); /* deleting のまま。次の機会に続ける */
            }
            job->waiting = true;
            return sres(ISO_STEP_CONTINUE, ISO_OK);
        }
        job->waiting = false;
        table_remove(c, job->id); /* カタログの分の参照を放す(0 になって解放される) */
        job->del.phase = 1;
    }
    iso_step_result_t r = iso_catalog_delete_step(c->cfg.cat, &job->del.dj);
    if (r.kind == ISO_STEP_CONTINUE) return r;
    if (r.kind == ISO_STEP_DONE) (void)iso_upload_refresh_free(&c->up);
    return end_job(c, r);
}

iso_step_result_t iso_job_step(iso_core_t *c, iso_job_t *job, uint32_t budget_bytes) {
    if (!c || !job || !c->cur_active || job != &c->cur) return sres(ISO_STEP_ERROR, ISO_ERR_INVALID);
    switch (job->kind) {
    case ISO_JOB_IDLE_CLOSE: {
        int rc = iso_upload_idle_close(&c->up, job->id);
        return end_job(c, sres(rc < 0 ? ISO_STEP_ERROR : ISO_STEP_DONE, rc < 0 ? rc : ISO_OK));
    }
    case ISO_JOB_FINISH: {
        iso_step_result_t r = iso_upload_finish_step(&c->up, &job->fin, budget_bytes, job->aborted);
        if (r.kind == ISO_STEP_CONTINUE) return r;
        return end_job(c, r);
    }
    case ISO_JOB_INDEX:
        if (job->aborted) return end_job(c, sres(ISO_STEP_DONE, ISO_OK));
        return step_index(c, job);
    case ISO_JOB_DELETE: return step_delete(c, job);
    default: return end_job(c, sres(ISO_STEP_ERROR, ISO_ERR_INVALID));
    }
}

/* ---- アイドルのタイムアウト ----------------------------------------------------------------- */

void iso_upload_tick(iso_core_t *c, uint64_t now_ms) {
    iso_upload_t *u = &c->up;
    if (!u->slot_active) return;
    if (u->slot_finalizing) {
        if (c->pub_act != ISO_UP_ACT_FINALIZING) publish_at(c, now_ms);
        return;
    }
    if (now_ms < u->last_put_ms) u->last_put_ms = now_ms; /* 巻き戻り: 経過 0。基準を引き直す */
    uint64_t elapsed = now_ms - u->last_put_ms;
    if (elapsed >= (uint64_t)ISO_UPLOAD_IDLE_S * 1000u) (void)iso_core_submit(c, ISO_JOB_IDLE_CLOSE, u->slot_id);
    if (iso_upload_activity(u, now_ms) != c->pub_act) publish_at(c, now_ms);
}
