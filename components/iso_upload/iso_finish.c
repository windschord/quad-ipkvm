#include "iso_upload_priv.h"

#include <string.h>

enum { FP_START = 0, FP_CLOSE, FP_HASH, FP_VERIFY, FP_SIG, FP_INDEX, FP_META, FP_DONE };

#define SIG_OFFSET (16u * ISO_CD_BLOCK_BYTES) /* セクタ 16 (CD のブロック 16) */

static iso_step_result_t step_res(iso_step_t kind, int err) {
    iso_step_result_t r = {kind, err};
    return r;
}

int iso_finish_job_init(iso_finish_job_t *job, const char *id) {
    if (!job || !iso_id_valid(id)) return ISO_ERR_INVALID;
    memset(job, 0, sizeof(*job));
    memcpy(job->id, id, ISO_ID_LEN + 1);
    job->phase = FP_START;
    return iso_index_job_init(&job->idx, id);
}

void iso_finish_job_release(iso_upload_t *u, iso_finish_job_t *job) { iso_index_job_release(u, &job->idx); }

/* ---- SHA-256 の再計算 ---------------------------------------------------------------- */

static iso_step_result_t hash_fail(iso_finish_job_t *job, iso_outcome_t kind, uint8_t reason, int err) {
    job->fail = kind;
    job->fail_reason = reason;
    job->fail_err = err;
    return step_res(ISO_STEP_DONE, ISO_OK);
}

iso_step_result_t iso_upload_hash_step(iso_upload_t *u, iso_finish_job_t *job, uint32_t budget_bytes) {
    const iso_fs_t *fs = iso_up_fs(u);
    const iso_sha256_t *sha = u->cfg.sha;
    iso_catalog_entry_t *e = iso_catalog_find(u->cfg.cat, job->id);
    if (!e || !e->meta_valid) return hash_fail(job, ISO_OUTCOME_IOERR, 0, ISO_ERR_NOT_FOUND);
    const iso_meta_t *m = &e->meta;

    if (!job->hash_started) {
        iso_up_discard_sha(u); /* 作業用に、同じ状態の領域を使う */
        sha->init(u->cfg.sha_state);
        job->hash_started = true;
        job->hash_part = 0;
        job->hash_off = 0;
        job->have_digest = false;
        job->fail = ISO_OUTCOME_NONE;
    }
    const uint64_t n_parts = iso_extent_part_count(m->size_bytes, m->part_blocks);
    const uint64_t expected = iso_extent_part_bytes(m->size_bytes, m->part_blocks, job->hash_part);
    char path[ISO_PATH_MAX];
    int rc = iso_up_part_path(path, sizeof(path), job->id, job->hash_part);
    if (rc < 0) return hash_fail(job, ISO_OUTCOME_IOERR, 0, rc);
    iso_file_t *f = NULL;
    rc = fs->open_for_read(fs->ctx, path, &f);
    if (rc == ISO_ERR_NOT_FOUND) return hash_fail(job, ISO_OUTCOME_INVALID, ISO_INVALID_PART_MISMATCH, 0);
    if (rc < 0) return hash_fail(job, ISO_OUTCOME_IOERR, 0, rc);

    const uint64_t limit = budget_bytes ? budget_bytes : u->cfg.io_buf_size;
    uint64_t spent = 0;
    while (job->hash_off < expected && spent < limit) {
        uint64_t n = expected - job->hash_off;
        if (n > u->cfg.io_buf_size) n = u->cfg.io_buf_size;
        if (n > limit - spent) n = limit - spent;
        size_t got = 0;
        rc = fs->read_at(fs->ctx, f, job->hash_off, u->cfg.io_buf, (size_t)n, &got);
        if (rc < 0) {
            (void)fs->close(fs->ctx, f);
            return hash_fail(job, ISO_OUTCOME_IOERR, 0, rc);
        }
        if (got != n) { /* パーツが、meta の期待より短い */
            (void)fs->close(fs->ctx, f);
            return hash_fail(job, ISO_OUTCOME_INVALID, ISO_INVALID_PART_MISMATCH, 0);
        }
        sha->update(u->cfg.sha_state, u->cfg.io_buf, (size_t)n);
        job->hash_off += n;
        spent += n;
    }
    (void)fs->close(fs->ctx, f);

    if (job->hash_off >= expected) {
        job->hash_part++;
        job->hash_off = 0;
        if (job->hash_part >= n_parts) {
            sha->final(u->cfg.sha_state, job->digest);
            job->have_digest = true;
            return step_res(ISO_STEP_DONE, ISO_OK);
        }
    }
    return step_res(ISO_STEP_CONTINUE, ISO_OK);
}

/* ---- 署名 ---------------------------------------------------------------------------- */

/* セクタ 16 のバイト 1〜5 が CD001 / BEA01 か。パーツがなければ ISO_ERR_NOT_FOUND。 */
static int check_signature(iso_upload_t *u, const iso_catalog_entry_t *e, bool *ok) {
    const iso_fs_t *fs = iso_up_fs(u);
    *ok = false;
    const uint64_t part_bytes = iso_up_part_bytes_full(&e->meta);
    char path[ISO_PATH_MAX];
    int rc = iso_up_part_path(path, sizeof(path), e->id, (uint32_t)(SIG_OFFSET / part_bytes));
    if (rc < 0) return rc;
    iso_file_t *f = NULL;
    rc = fs->open_for_read(fs->ctx, path, &f);
    if (rc < 0) return rc;
    uint8_t buf[6];
    size_t got = 0;
    rc = fs->read_at(fs->ctx, f, SIG_OFFSET % part_bytes, buf, sizeof(buf), &got);
    (void)fs->close(fs->ctx, f);
    if (rc < 0) return rc;
    if (got == sizeof(buf) && (memcmp(buf + 1, "CD001", 5) == 0 || memcmp(buf + 1, "BEA01", 5) == 0)) *ok = true;
    return ISO_OK;
}

/* ---- 終わり方 ------------------------------------------------------------------------- */

/* 検査に失敗: invalid(理由を meta に保存)。枠を空け、SHA-256 の状態を捨てる。meta の書き込みが失敗したら ERROR(メモリ上は invalid) */
static iso_step_result_t end_invalid(iso_upload_t *u, iso_finish_job_t *job, iso_catalog_entry_t *e, uint8_t reason) {
    iso_apply_arg_t arg;
    memset(&arg, 0, sizeof(arg));
    arg.invalid_reason = reason;
    int rc = iso_catalog_apply(u->cfg.cat, e, ISO_EV_FINALIZE_FAIL, NULL, &arg);
    if (rc != 0) {
        e->state = ISO_STATE_INVALID;
        e->invalid_reason = reason;
    }
    iso_finish_job_release(u, job);
    iso_up_release_slot(u);
    iso_up_discard_sha(u);
    job->phase = FP_DONE;
    (void)iso_upload_refresh_free(u);
    iso_up_notify(u);
    return step_res(rc < 0 ? ISO_STEP_ERROR : ISO_STEP_DONE, rc < 0 ? rc : ISO_OK);
}

/* SD の I/O エラー: incomplete(received は実ファイルの大きさから決める)。再試行は finish。 */
static iso_step_result_t end_ioerr(iso_upload_t *u, iso_finish_job_t *job, iso_catalog_entry_t *e, int err) {
    (void)iso_up_close_file(u, false);
    (void)iso_upload_reload_received(u, e); /* 失敗したら、そのまま(finish の受付が、ディスクから決め直す) */
    (void)iso_up_force_incomplete(u, e, ISO_EV_FINALIZE_IOERR);
    iso_finish_job_release(u, job);
    iso_up_release_slot(u);
    job->phase = FP_DONE;
    iso_up_notify(u);
    return step_res(ISO_STEP_ERROR, err);
}

static iso_step_result_t from_fail(iso_upload_t *u, iso_finish_job_t *job, iso_catalog_entry_t *e, iso_outcome_t kind, uint8_t reason,
                                   int err) {
    if (kind == ISO_OUTCOME_INVALID) return end_invalid(u, job, e, reason);
    return end_ioerr(u, job, e, err);
}

iso_step_result_t iso_upload_finish_step(iso_upload_t *u, iso_finish_job_t *job, uint32_t budget_bytes, bool aborted) {
    if (aborted) {
        iso_finish_job_release(u, job);
        job->phase = FP_DONE;
        return step_res(ISO_STEP_DONE, ISO_OK);
    }
    iso_catalog_entry_t *e = iso_catalog_find(u->cfg.cat, job->id);
    if (job->phase == FP_DONE || !e || !e->meta_valid || e->state != ISO_STATE_FINALIZING || !u->slot_active ||
        !u->slot_finalizing || strcmp(u->slot_id, job->id) != 0) {
        iso_finish_job_release(u, job);
        job->phase = FP_DONE;
        return step_res(ISO_STEP_DONE, ISO_OK);
    }
    const iso_meta_t *m = &e->meta;

    switch (job->phase) {
    case FP_START:
        job->phase = FP_CLOSE;
        return step_res(ISO_STEP_CONTINUE, ISO_OK);

    case FP_CLOSE: {
        /* FatFs の排他: 書き込み用のハンドルを先に閉じる(sync してから) */
        int rc = iso_up_close_file(u, true);
        if (rc < 0) return end_ioerr(u, job, e, rc);
        bool streaming = u->sha_valid && strcmp(u->sha_id, job->id) == 0 && u->sha_bytes == m->size_bytes;
        job->phase = streaming ? FP_VERIFY : FP_HASH;
        return step_res(ISO_STEP_CONTINUE, ISO_OK);
    }

    case FP_HASH: {
        (void)iso_upload_hash_step(u, job, budget_bytes);
        if (job->fail != ISO_OUTCOME_NONE) return from_fail(u, job, e, job->fail, job->fail_reason, job->fail_err);
        if (job->have_digest) job->phase = FP_VERIFY;
        return step_res(ISO_STEP_CONTINUE, ISO_OK);
    }

    case FP_VERIFY: {
        if (!job->have_digest) {
            u->cfg.sha->final(u->cfg.sha_state, job->digest); /* 逐次の状態を確定する(状態は、これで使えなくなる) */
            job->have_digest = true;
        }
        iso_up_discard_sha(u);
        if (m->sha_flag == ISO_SHA_DECLARED && memcmp(m->sha256, job->digest, ISO_SHA256_LEN) != 0)
            return end_invalid(u, job, e, ISO_INVALID_CHECKSUM_MISMATCH);
        job->phase = FP_SIG;
        return step_res(ISO_STEP_CONTINUE, ISO_OK);
    }

    case FP_SIG: {
        bool ok = false;
        int rc = check_signature(u, e, &ok);
        if (rc == ISO_ERR_NOT_FOUND) return end_invalid(u, job, e, ISO_INVALID_PART_MISMATCH);
        if (rc < 0) return end_ioerr(u, job, e, rc);
        if (!ok) return end_invalid(u, job, e, ISO_INVALID_NOT_ISO);
        job->phase = FP_INDEX;
        return step_res(ISO_STEP_CONTINUE, ISO_OK);
    }

    case FP_INDEX: {
        iso_step_result_t r = iso_index_step(u, &job->idx);
        if (r.kind == ISO_STEP_CONTINUE) return r;
        if (job->idx.outcome == ISO_OUTCOME_OK) {
            job->phase = FP_META;
            return step_res(ISO_STEP_CONTINUE, ISO_OK);
        }
        return from_fail(u, job, e, job->idx.outcome, job->idx.reason, job->idx.err);
    }

    case FP_META:
    default: {
        iso_apply_arg_t arg;
        memset(&arg, 0, sizeof(arg));
        arg.set_sha = true;
        arg.sha_flag = ISO_SHA_COMPUTED;
        arg.sha256 = job->digest;
        int rc = iso_catalog_apply(u->cfg.cat, e, ISO_EV_FINALIZE_OK, NULL, &arg);
        if (rc != 0) return end_ioerr(u, job, e, rc < 0 ? rc : ISO_ERR_INVALID);
        /* 表を引き渡す(所有権が移る)。NULL のフックなら、受け取らずに放す */
        iso_extent_table_t *t = job->idx.table;
        job->idx.table = NULL;
        if (u->cfg.hooks.table_ready) {
            u->cfg.hooks.table_ready(u->cfg.hooks.ctx, job->id, t);
        } else {
            (void)iso_extent_table_unref(t);
        }
        iso_up_release_slot(u);
        job->phase = FP_DONE;
        (void)iso_upload_refresh_free(u);
        iso_up_notify(u);
        return step_res(ISO_STEP_DONE, ISO_OK);
    }
    }
}
