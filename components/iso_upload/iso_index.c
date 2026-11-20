#include "iso_upload_priv.h"

#include <string.h>

enum { IX_INIT = 0, IX_PARTS, IX_BUILD, IX_DONE };

static iso_step_result_t res(iso_step_t kind) {
    iso_step_result_t r = {kind, ISO_OK};
    return r;
}

int iso_index_job_init(iso_index_job_t *job, const char *id) {
    if (!job || !iso_id_valid(id)) return ISO_ERR_INVALID;
    memset(job, 0, sizeof(*job));
    memcpy(job->id, id, ISO_ID_LEN + 1);
    job->phase = IX_INIT;
    return ISO_OK;
}

static void release_pool(iso_upload_t *u, iso_index_job_t *job) {
    if (job->pool) {
        u->cfg.alloc->free(u->cfg.alloc->ctx, job->pool);
        job->pool = NULL;
    }
}

void iso_index_job_release(iso_upload_t *u, iso_index_job_t *job) {
    release_pool(u, job);
    if (job->table) {
        (void)iso_extent_table_unref(job->table);
        job->table = NULL;
    }
}

/* 終了。outcome を決めて、作業領域を解放する(表は残す) */
static iso_step_result_t finish_invalid(iso_upload_t *u, iso_index_job_t *job, uint8_t reason) {
    job->outcome = ISO_OUTCOME_INVALID;
    job->reason = reason;
    iso_index_job_release(u, job);
    job->phase = IX_DONE;
    return res(ISO_STEP_DONE);
}

static iso_step_result_t finish_ioerr(iso_upload_t *u, iso_index_job_t *job, int err) {
    job->outcome = ISO_OUTCOME_IOERR;
    job->err = err;
    iso_index_job_release(u, job);
    job->phase = IX_DONE;
    return res(ISO_STEP_DONE);
}

iso_step_result_t iso_index_step(iso_upload_t *u, iso_index_job_t *job) {
    const iso_fs_t *fs = iso_up_fs(u);
    iso_catalog_entry_t *e = iso_catalog_find(u->cfg.cat, job->id);
    if (job->phase == IX_DONE) return res(ISO_STEP_DONE);
    if (!e || !e->meta_valid || (e->state != ISO_STATE_READY && e->state != ISO_STATE_FINALIZING))
        return finish_ioerr(u, job, ISO_ERR_NOT_FOUND);
    const iso_meta_t *m = &e->meta;

    if (job->phase == IX_INIT) {
        uint64_t n = iso_extent_part_count(m->size_bytes, m->part_blocks);
        if (n == 0 || n > ISO_MAX_PARTS || n != m->part_count) return finish_invalid(u, job, ISO_INVALID_PART_MISMATCH);
        job->n_parts = (uint32_t)n;
        job->epoch = u->cfg.bdev ? u->cfg.bdev->epoch(u->cfg.bdev->ctx) : 0; /* get_extents より前に読む(途中でカードが替わったら、表は無効になる) */
        job->pool = (iso_extent_t *)u->cfg.alloc->alloc(u->cfg.alloc->ctx, sizeof(iso_extent_t) * ISO_INDEX_POOL);
        if (!job->pool) return finish_ioerr(u, job, ISO_ERR_NOMEM);
        job->pool_used = 0;
        job->part = 0;
        job->phase = IX_PARTS;
        return res(ISO_STEP_CONTINUE);
    }

    if (job->phase == IX_PARTS) {
        char path[ISO_PATH_MAX];
        int rc = iso_up_part_path(path, sizeof(path), job->id, job->part);
        if (rc < 0) return finish_ioerr(u, job, rc);
        iso_stat_t st;
        rc = fs->stat(fs->ctx, path, &st);
        if (rc == ISO_ERR_NOT_FOUND) return finish_invalid(u, job, ISO_INVALID_PART_MISMATCH); /* パーツの欠落 */
        if (rc < 0) return finish_ioerr(u, job, rc);
        if (st.is_dir) return finish_invalid(u, job, ISO_INVALID_PART_MISMATCH);
        size_t n = 0;
        rc = fs->get_extents(fs->ctx, path, job->pool + job->pool_used, ISO_INDEX_POOL - job->pool_used, &n);
        if (rc == ISO_ERR_TOO_FRAGMENTED) return finish_invalid(u, job, ISO_INVALID_TOO_FRAGMENTED);
        if (rc == ISO_ERR_NOT_FOUND) return finish_invalid(u, job, ISO_INVALID_PART_MISMATCH);
        if (rc < 0) return finish_ioerr(u, job, rc);
        job->inputs[job->part].file_bytes = st.size;
        job->inputs[job->part].extents = job->pool + job->pool_used;
        job->inputs[job->part].n_extents = n;
        job->pool_used += n;
        job->part++;
        if (job->part >= job->n_parts) job->phase = IX_BUILD;
        return res(ISO_STEP_CONTINUE);
    }

    /* IX_BUILD */
    iso_extent_table_t *t = NULL;
    int rc = iso_extent_table_build(u->cfg.alloc, job->epoch, m->size_bytes, m->part_blocks, job->inputs, job->n_parts, &t);
    if (rc == ISO_EXT_OK) {
        job->table = t;
        job->outcome = ISO_OUTCOME_OK;
        release_pool(u, job); /* 作業領域だけ解放(表は残る) */
        job->phase = IX_DONE;
        return res(ISO_STEP_DONE);
    }
    if (rc == ISO_EXT_ERR_TOO_FRAGMENTED) return finish_invalid(u, job, ISO_INVALID_TOO_FRAGMENTED);
    if (rc == ISO_EXT_ERR_NOMEM) return finish_ioerr(u, job, ISO_ERR_NOMEM);
    return finish_invalid(u, job, ISO_INVALID_PART_MISMATCH); /* PART_MISMATCH、パラメーター・エクステントの不正 */
}
