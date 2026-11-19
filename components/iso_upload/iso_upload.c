#include "iso_upload_priv.h"

#include <string.h>

static uint64_t now_ms(const iso_upload_t *u) { return u->cfg.clock->now_ms(u->cfg.clock->ctx); }

int iso_upload_init(iso_upload_t *u, const iso_upload_cfg_t *cfg) {
    if (!u || !cfg || !cfg->cat || !cfg->cat->fs || !cfg->clock || !cfg->clock->now_ms || !cfg->sha || !cfg->sha_state ||
        !cfg->io_buf || cfg->io_buf_size == 0 || !cfg->alloc)
        return ISO_ERR_INVALID;
    memset(u, 0, sizeof(*u));
    u->cfg = *cfg;
    if (u->cfg.chunk_bytes == 0) u->cfg.chunk_bytes = ISO_UPLOAD_CHUNK_KB * 1024u;
    if (u->cfg.sync_bytes == 0) u->cfg.sync_bytes = ISO_SYNC_KIB * 1024u;
    return ISO_OK;
}

/* ---- 枠、SHA-256、ファイル ------------------------------------------------------------- */

void iso_up_release_slot(iso_upload_t *u) {
    u->slot_active = false;
    u->slot_finalizing = false;
    u->slot_id[0] = '\0';
}

void iso_up_discard_sha(iso_upload_t *u) {
    u->sha_valid = false;
    u->sha_id[0] = '\0';
    u->sha_bytes = 0;
}

static void take_slot(iso_upload_t *u, const char *id, bool finalizing) {
    u->slot_active = true;
    u->slot_finalizing = finalizing;
    memcpy(u->slot_id, id, ISO_ID_LEN + 1);
}

int iso_up_close_file(iso_upload_t *u, bool do_sync) {
    if (!u->f) return ISO_OK;
    const iso_fs_t *fs = iso_up_fs(u);
    int rc = ISO_OK;
    if (do_sync) rc = fs->sync(fs->ctx, u->f);
    int c = fs->close(fs->ctx, u->f);
    u->f = NULL;
    u->since_sync = 0;
    return rc < 0 ? rc : c;
}

int iso_up_force_incomplete(iso_upload_t *u, iso_catalog_entry_t *e, iso_event_t ev) {
    int rc = iso_catalog_apply(u->cfg.cat, e, ev, NULL, NULL);
    if (rc == 0) return ISO_OK;
    if (e->state == ISO_STATE_UPLOADING || e->state == ISO_STATE_FINALIZING) e->state = ISO_STATE_INCOMPLETE;
    return rc < 0 ? rc : ISO_OK;
}

/* ---- 空き容量 ------------------------------------------------------------------------- */

void iso_upload_set_free(iso_upload_t *u, uint64_t total_bytes, uint64_t free_bytes) {
    u->free_known = true;
    u->total_bytes = total_bytes;
    u->free_bytes = free_bytes;
    u->consumed_since = 0;
}

int iso_upload_refresh_free(iso_upload_t *u) {
    const iso_fs_t *fs = iso_up_fs(u);
    iso_mount_info_t mi;
    memset(&mi, 0, sizeof(mi));
    int rc = fs->mount_info(fs->ctx, &mi);
    if (rc < 0) return rc;
    iso_upload_set_free(u, mi.total_bytes, mi.free_bytes);
    return ISO_OK;
}

/* uploading と incomplete のイメージの、これから使う分(size_bytes - received)の合計。skip は数えない。 */
static uint64_t pending_bytes(const iso_catalog_t *cat, const iso_catalog_entry_t *skip) {
    uint64_t sum = 0;
    for (size_t i = 0; i < cat->count; i++) {
        const iso_catalog_entry_t *e = &cat->images[i];
        if (e == skip || !e->meta_valid) continue;
        if (e->state != ISO_STATE_UPLOADING && e->state != ISO_STATE_INCOMPLETE) continue;
        if (e->received < e->meta.size_bytes) sum += e->meta.size_bytes - e->received;
    }
    return sum;
}

static uint64_t avail_bytes(const iso_upload_t *u, const iso_catalog_entry_t *skip) {
    if (!u->free_known) return 0;
    uint64_t used = u->consumed_since + pending_bytes(u->cfg.cat, skip);
    return u->free_bytes > used ? u->free_bytes - used : 0;
}

uint64_t iso_upload_free_estimate(const iso_upload_t *u) { return avail_bytes(u, NULL); }

/* need バイト + 予備 ISO_FREE_RESERVE_MIB が入るか */
static bool have_space(const iso_upload_t *u, uint64_t need, const iso_catalog_entry_t *skip) {
    return avail_bytes(u, skip) >= need + (uint64_t)ISO_FREE_RESERVE_MIB * ISO_UP_MIB;
}

/* ---- begin ---------------------------------------------------------------------------- */

int iso_upload_begin(iso_upload_t *u, const char *name, size_t name_len, uint64_t size_bytes, const uint8_t *sha256,
                     char out_id[ISO_ID_LEN + 1]) {
    if (!u || !name || !out_id) return ISO_ERR_INVALID;
    if (!iso_name_valid(name, name_len)) return ISO_ERR_INVALID;
    if (size_bytes < ISO_IMAGE_BYTES_MIN || size_bytes > ISO_IMAGE_BYTES_MAX) return ISO_ERR_INVALID;
    iso_catalog_t *cat = u->cfg.cat;
    if (u->slot_active) return ISO_RES_UPLOAD_IN_PROGRESS;
    if (iso_catalog_name_exists(cat, name, name_len)) return ISO_RES_NAME_EXISTS;
    if (cat->count >= ISO_MAX_IMAGES) return ISO_RES_TOO_MANY_IMAGES;
    if (!u->free_known || size_bytes > u->total_bytes || !have_space(u, size_bytes, NULL)) return ISO_RES_INSUFFICIENT_STORAGE;

    int rc = iso_catalog_begin(cat, name, name_len, size_bytes, sha256, out_id);
    if (rc < 0) return rc;

    take_slot(u, out_id, false);
    u->last_put_ms = now_ms(u);
    u->cfg.sha->init(u->cfg.sha_state);
    u->sha_valid = true;
    memcpy(u->sha_id, out_id, ISO_ID_LEN + 1);
    u->sha_bytes = 0;
    iso_up_notify(u);
    return ISO_OK;
}

/* ---- 状態の問い合わせ -------------------------------------------------------------------- */

void iso_upload_info(const iso_upload_t *u, const char *id, iso_upload_info_t *out) {
    memset(out, 0, sizeof(*out));
    out->chunk_bytes = u->cfg.chunk_bytes;
    out->slot_free = !u->slot_active;
    iso_catalog_entry_t *e = iso_catalog_find(u->cfg.cat, id);
    if (!e || e->state == ISO_STATE_NONE || e->state == ISO_STATE_DELETING) return;
    out->found = true;
    out->state = e->state;
    out->meta_valid = e->meta_valid;
    out->size_bytes = e->meta_valid ? e->meta.size_bytes : 0;
    out->received = e->received;
    out->slot_owner = u->slot_active && strcmp(u->slot_id, id) == 0;
}

bool iso_upload_slot_busy(const iso_upload_t *u) { return u->slot_active; }
const char *iso_upload_slot_id(const iso_upload_t *u) { return u->slot_active ? u->slot_id : NULL; }
uint32_t iso_upload_chunk_bytes(const iso_upload_t *u) { return u->cfg.chunk_bytes; }

int iso_upload_reload_received(iso_upload_t *u, iso_catalog_entry_t *e) {
    uint64_t r = 0;
    int rc = iso_catalog_received(iso_up_fs(u), e->id, &e->meta, &r);
    if (rc < 0) return rc;
    e->received = r;
    return ISO_OK;
}

/* ---- チャンクの書き込み ----------------------------------------------------------------- */

/* パーツ part を書き込み用に開く。off = パーツの中の、書き始める位置(= 今のファイルの大きさ) */
static int open_part(iso_upload_t *u, const char *id, uint32_t part, uint64_t off) {
    const iso_fs_t *fs = iso_up_fs(u);
    char path[ISO_PATH_MAX];
    int rc = iso_up_part_path(path, sizeof(path), id, part);
    if (rc < 0) return rc;
    iso_file_t *f = NULL;
    if (off > 0) {
        rc = fs->open_for_write(fs->ctx, path, true, &f);
    } else {
        rc = fs->open_for_write(fs->ctx, path, false, &f);
        if (rc == ISO_ERR_EXISTS) {
            /* 前の失敗で残った、空でないかもしれない同名のファイル。空にしてから追記する */
            rc = fs->truncate(fs->ctx, path, 0);
            if (rc == ISO_OK) rc = fs->open_for_write(fs->ctx, path, true, &f);
        }
    }
    if (rc < 0) return rc;
    u->f = f;
    u->f_part = part;
    u->since_sync = 0;
    return ISO_OK;
}

typedef struct {
    uint32_t part;
    uint64_t before; /* このチャンクを書く前の、パーツの中の大きさ */
} touched_t;

/* 失敗の後始末: ファイルを閉じ、触ったパーツを書く前の大きさに戻す(新規は 0)。truncate が失敗したら received はディスクから決める。
 * incomplete にして、枠を空ける(SHA-256 の状態は、received と合っていれば残る)。 */
static int fail_write(iso_upload_t *u, iso_catalog_entry_t *e, uint64_t received_before, const touched_t *t, size_t nt, int err,
                      iso_upload_write_t *out) {
    const iso_fs_t *fs = iso_up_fs(u);
    (void)iso_up_close_file(u, false); /* 失敗した write の後でも閉じられる。close の結果は見ない */
    bool trunc_failed = false;
    for (size_t k = nt; k > 0; k--) {
        char path[ISO_PATH_MAX];
        if (iso_up_part_path(path, sizeof(path), e->id, t[k - 1].part) < 0 ||
            fs->truncate(fs->ctx, path, t[k - 1].before) < 0)
            trunc_failed = true;
    }
    e->received = received_before;
    if (trunc_failed) (void)iso_upload_reload_received(u, e); /* 失敗したら、received_before のまま(再開のときに、ディスクから決め直す) */
    (void)iso_up_force_incomplete(u, e, ISO_EV_WRITE_FAIL);
    iso_up_release_slot(u);
    u->since_sync = 0;
    if (out) out->received = e->received;
    iso_up_notify(u);
    return err == ISO_ERR_NO_SPACE ? (int)ISO_RES_INSUFFICIENT_STORAGE : err;
}

int iso_upload_write_chunk(iso_upload_t *u, const char *id, uint64_t offset, const void *data, size_t len, iso_upload_write_t *out) {
    if (out) out->received = 0;
    if (!u || !id || !data || len == 0) return ISO_ERR_INVALID;
    if (len > u->cfg.chunk_bytes) return ISO_RES_CHUNK_TOO_LARGE;
    iso_catalog_t *cat = u->cfg.cat;
    const iso_fs_t *fs = iso_up_fs(u);
    iso_catalog_entry_t *e = iso_catalog_find(cat, id);
    if (!e || e->state == ISO_STATE_NONE || e->state == ISO_STATE_DELETING) return ISO_RES_NOT_FOUND;

    bool resume = (e->state == ISO_STATE_INCOMPLETE);
    if (resume) {
        if (u->slot_active) return ISO_RES_UPLOAD_IN_PROGRESS;
    } else if (e->state != ISO_STATE_UPLOADING) {
        return ISO_RES_NOT_UPLOADING;
    }
    /* ファイルを開いていない(再開、アイドルの後、パーツの終わりの後)ときは、ディスク上の大きさから決め直す */
    if (resume || !u->f) {
        int rc = iso_upload_reload_received(u, e);
        if (rc < 0) return rc;
    }
    if (out) out->received = e->received;
    if (offset != e->received) return ISO_RES_OFFSET_MISMATCH;
    if (len > e->meta.size_bytes - offset) return ISO_RES_EXCEEDS_SIZE;
    if (resume) {
        if (!have_space(u, e->meta.size_bytes - e->received, e)) return ISO_RES_INSUFFICIENT_STORAGE;
        iso_trans_ctx_t tc = {false, true, false};
        int rc = iso_catalog_apply(cat, e, ISO_EV_PUT, &tc, NULL); /* incomplete -> uploading。meta は書かない */
        if (rc != 0) return rc;
        take_slot(u, id, false);
    }
    bool sha_cont = u->sha_valid && strcmp(u->sha_id, id) == 0 && u->sha_bytes == e->received;
    if (!sha_cont) iso_up_discard_sha(u);

    const uint64_t part_bytes = iso_up_part_bytes_full(&e->meta);
    const uint64_t received_before = e->received;
    touched_t touched[ISO_MAX_PARTS];
    size_t nt = 0;
    uint64_t pos = received_before;
    size_t done = 0;
    const uint8_t *p = (const uint8_t *)data;
    while (done < len) {
        uint32_t part = (uint32_t)(pos / part_bytes);
        uint64_t off = pos % part_bytes;
        uint64_t seg = part_bytes - off;
        if (seg > len - done) seg = len - done;
        int rc = ISO_OK;
        if (!u->f) {
            rc = open_part(u, id, part, off);
            if (rc < 0) return fail_write(u, e, received_before, touched, nt, rc, out);
        }
        if (nt == 0 || touched[nt - 1].part != part) {
            touched[nt].part = part;
            touched[nt].before = off;
            nt++;
        }
        rc = fs->write(fs->ctx, u->f, p + done, (size_t)seg);
        if (rc < 0) return fail_write(u, e, received_before, touched, nt, rc, out);
        pos += seg;
        done += (size_t)seg;
        u->since_sync += seg;
        if (off + seg == part_bytes) {
            rc = iso_up_close_file(u, true); /* パーツの終わり: sync して閉じる(次のパーツは新規作成) */
            if (rc < 0) return fail_write(u, e, received_before, touched, nt, rc, out);
        } else if (u->since_sync >= u->cfg.sync_bytes) {
            rc = fs->sync(fs->ctx, u->f);
            if (rc < 0) return fail_write(u, e, received_before, touched, nt, rc, out);
            u->since_sync = 0;
        }
    }

    e->received = pos;
    if (u->sha_valid) {
        u->cfg.sha->update(u->cfg.sha_state, data, len);
        u->sha_bytes += len;
    }
    u->consumed_since += len;
    u->last_put_ms = now_ms(u);
    if (out) out->received = e->received;
    iso_up_notify(u);
    return ISO_OK;
}

/* ---- アイドル、中止、finish の受付 ------------------------------------------------------ */

int iso_upload_idle_close(iso_upload_t *u, const char *id) {
    if (!u->slot_active || u->slot_finalizing || strcmp(u->slot_id, id) != 0) return ISO_OK;
    iso_catalog_entry_t *e = iso_catalog_find(u->cfg.cat, id);
    if (!e || e->state != ISO_STATE_UPLOADING) return ISO_OK;

    int rc = iso_up_close_file(u, true);
    if (rc < 0) (void)iso_upload_reload_received(u, e); /* 同期できたか分からない: ディスクの大きさに合わせる */
    int mrc = iso_up_force_incomplete(u, e, ISO_EV_IDLE_TIMEOUT);
    iso_up_release_slot(u);
    iso_up_notify(u);
    return rc < 0 ? rc : mrc;
}

void iso_upload_abort(iso_upload_t *u, const char *id) {
    if (!u->slot_active || strcmp(u->slot_id, id) != 0) return;
    (void)iso_up_close_file(u, false);
    if (strcmp(u->sha_id, id) == 0) iso_up_discard_sha(u);
    iso_up_release_slot(u);
    iso_up_notify(u);
}

int iso_upload_finish_begin(iso_upload_t *u, const char *id, uint64_t *out_received) {
    iso_catalog_t *cat = u->cfg.cat;
    iso_catalog_entry_t *e = iso_catalog_find(cat, id);
    if (!e || e->state == ISO_STATE_NONE || e->state == ISO_STATE_DELETING) return ISO_RES_NOT_FOUND;
    switch (e->state) {
    case ISO_STATE_READY: return ISO_RES_ALREADY_READY;
    case ISO_STATE_FINALIZING: return ISO_RES_ALREADY_FINALIZING;
    case ISO_STATE_INVALID: return ISO_RES_INVALID_IMAGE;
    default: break;
    }
    if (e->state == ISO_STATE_INCOMPLETE || !u->f) {
        int rc = iso_upload_reload_received(u, e);
        if (rc < 0) return rc;
    }
    if (out_received) *out_received = e->received;
    iso_trans_ctx_t tc = {false, !u->slot_active, false};
    int rc = iso_catalog_apply(cat, e, ISO_EV_FINISH, &tc, NULL);
    if (rc < 0) return rc;
    if (rc == (int)ISO_TR_REJECT_SIZE_MISMATCH) return ISO_RES_SIZE_MISMATCH;
    if (rc == (int)ISO_TR_REJECT_UPLOAD_IN_PROGRESS) return ISO_RES_UPLOAD_IN_PROGRESS;
    if (rc > 0) return ISO_RES_NOT_UPLOADING;
    take_slot(u, id, true);
    iso_up_notify(u);
    return ISO_OK;
}

/* ---- アクティビティ、SD の再初期化 -------------------------------------------------------- */

iso_upload_activity_t iso_upload_activity_from(uint64_t last_put_ms, uint64_t now) {
    uint64_t elapsed = now >= last_put_ms ? now - last_put_ms : 0;
    return elapsed <= (uint64_t)ISO_UPLOAD_ACTIVE_S * 1000u ? ISO_UP_ACT_RECEIVING : ISO_UP_ACT_IDLE;
}

iso_upload_activity_t iso_upload_activity(const iso_upload_t *u, uint64_t now) {
    if (!u->slot_active) return ISO_UP_ACT_NONE;
    if (u->slot_finalizing) return ISO_UP_ACT_FINALIZING;
    return iso_upload_activity_from(u->last_put_ms, now);
}

void iso_upload_reinit(iso_upload_t *u) {
    if (u->f) {
        const iso_fs_t *fs = iso_up_fs(u);
        (void)fs->close(fs->ctx, u->f); /* カードが抜かれたかもしれない。エラーは見ない */
        u->f = NULL;
    }
    u->since_sync = 0;
    iso_up_discard_sha(u);
    iso_up_release_slot(u);
    iso_catalog_t *cat = u->cfg.cat;
    for (size_t i = 0; i < cat->count; i++) {
        iso_catalog_entry_t *e = &cat->images[i];
        if (e->state == ISO_STATE_UPLOADING || e->state == ISO_STATE_FINALIZING) e->state = ISO_STATE_INCOMPLETE;
    }
}
