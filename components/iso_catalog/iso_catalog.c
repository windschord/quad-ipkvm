#include "iso_catalog.h"

#include <stdio.h>
#include <string.h>

#define ROOT_DIR "/iso"
#define IMPORT_DIR "/iso/import"

/* ---- パス ---------------------------------------------------------------- */

static int path_check(int n, size_t cap) { return (n < 0 || (size_t)n >= cap) ? ISO_ERR_INVALID : ISO_OK; }

static int path_dir(char *buf, size_t cap, const char *id) { return path_check(snprintf(buf, cap, "/iso/%s", id), cap); }
static int path_slot(char *buf, size_t cap, const char *id, int slot) {
    return path_check(snprintf(buf, cap, "/iso/%s/meta.%c", id, slot == 0 ? 'a' : 'b'), cap);
}
static int path_part(char *buf, size_t cap, const char *id, unsigned idx) {
    return path_check(snprintf(buf, cap, "/iso/%s/p%03u", id, idx), cap);
}

/* ---- 遷移表 -------------------------------------------------------------- */

iso_trans_t iso_state_transition(iso_state_t cur, iso_event_t ev, const iso_trans_ctx_t *ctx, iso_state_t *next) {
    static const iso_trans_ctx_t zero = {false, false, false};
    const iso_trans_ctx_t *c = ctx ? ctx : &zero;
    iso_state_t n = cur;
    iso_trans_t r = ISO_TR_REJECT_STATE;

    switch (ev) {
    case ISO_EV_BEGIN:
        if (cur == ISO_STATE_NONE) {
            n = ISO_STATE_UPLOADING;
            r = ISO_TR_OK;
        }
        break;
    case ISO_EV_PUT:
        if (cur == ISO_STATE_UPLOADING) {
            r = ISO_TR_OK;
        } else if (cur == ISO_STATE_INCOMPLETE) {
            if (c->slot_free) {
                n = ISO_STATE_UPLOADING;
                r = ISO_TR_OK;
            } else {
                r = ISO_TR_REJECT_UPLOAD_IN_PROGRESS;
            }
        }
        break;
    case ISO_EV_IDLE_TIMEOUT:
    case ISO_EV_WRITE_FAIL:
        if (cur == ISO_STATE_UPLOADING) {
            n = ISO_STATE_INCOMPLETE;
            r = ISO_TR_OK;
        }
        break;
    case ISO_EV_REBOOT:
        switch (cur) {
        case ISO_STATE_UPLOADING:
        case ISO_STATE_FINALIZING:
            n = ISO_STATE_INCOMPLETE;
            r = ISO_TR_OK;
            break;
        case ISO_STATE_INCOMPLETE:
        case ISO_STATE_READY:
        case ISO_STATE_INVALID:
        case ISO_STATE_DELETING:
            r = ISO_TR_OK;
            break;
        default:
            break;
        }
        break;
    case ISO_EV_FINISH:
        switch (cur) {
        case ISO_STATE_UPLOADING:
        case ISO_STATE_INCOMPLETE:
            if (!c->received_complete) {
                r = ISO_TR_REJECT_SIZE_MISMATCH;
            } else if (cur == ISO_STATE_INCOMPLETE && !c->slot_free) {
                r = ISO_TR_REJECT_UPLOAD_IN_PROGRESS;
            } else {
                n = ISO_STATE_FINALIZING;
                r = ISO_TR_OK;
            }
            break;
        case ISO_STATE_FINALIZING: /* 冪等 */
        case ISO_STATE_READY:      /* 冪等 */
            r = ISO_TR_OK;
            break;
        case ISO_STATE_INVALID:
            r = ISO_TR_REJECT_INVALID;
            break;
        default:
            break;
        }
        break;
    case ISO_EV_FINALIZE_OK:
        if (cur == ISO_STATE_FINALIZING) {
            n = ISO_STATE_READY;
            r = ISO_TR_OK;
        }
        break;
    case ISO_EV_FINALIZE_FAIL:
        if (cur == ISO_STATE_FINALIZING) {
            n = ISO_STATE_INVALID;
            r = ISO_TR_OK;
        }
        break;
    case ISO_EV_FINALIZE_IOERR:
        if (cur == ISO_STATE_FINALIZING) {
            n = ISO_STATE_INCOMPLETE;
            r = ISO_TR_OK;
        }
        break;
    case ISO_EV_INDEX_OK:
    case ISO_EV_INDEX_IOERR:
        if (cur == ISO_STATE_READY) r = ISO_TR_OK;
        break;
    case ISO_EV_INDEX_FAIL:
        if (cur == ISO_STATE_READY) {
            n = ISO_STATE_INVALID;
            r = ISO_TR_OK;
        }
        break;
    case ISO_EV_DELETE:
        if (cur != ISO_STATE_NONE) {
            if (cur == ISO_STATE_READY && c->mounted) {
                r = ISO_TR_REJECT_MOUNTED;
            } else {
                n = ISO_STATE_DELETING;
                r = ISO_TR_OK;
            }
        }
        break;
    case ISO_EV_DELETE_DONE:
        if (cur == ISO_STATE_DELETING) {
            n = ISO_STATE_NONE;
            r = ISO_TR_OK;
        }
        break;
    default:
        break;
    }
    if (next) *next = (r == ISO_TR_OK) ? n : cur;
    return r;
}

int iso_state_to_meta(iso_state_t s) {
    switch (s) {
    case ISO_STATE_UPLOADING: return ISO_META_UPLOADING;
    case ISO_STATE_READY: return ISO_META_READY;
    case ISO_STATE_INCOMPLETE: return ISO_META_INCOMPLETE;
    case ISO_STATE_INVALID: return ISO_META_INVALID;
    case ISO_STATE_DELETING: return ISO_META_DELETING;
    default: return -1;
    }
}

iso_state_t iso_state_from_meta(uint8_t meta_state) {
    switch (meta_state) {
    case ISO_META_UPLOADING: return ISO_STATE_UPLOADING;
    case ISO_META_READY: return ISO_STATE_READY;
    case ISO_META_INCOMPLETE: return ISO_STATE_INCOMPLETE;
    case ISO_META_INVALID: return ISO_STATE_INVALID;
    case ISO_META_DELETING: return ISO_STATE_DELETING;
    default: return ISO_STATE_NONE;
    }
}

/* ---- 2 スロット ----------------------------------------------------------- */

int iso_slot_choose(bool a_valid, uint32_t a_gen, bool b_valid, uint32_t b_gen) {
    if (!a_valid && !b_valid) return -1;
    if (a_valid && !b_valid) return 0;
    if (!a_valid && b_valid) return 1;
    return ((int32_t)(b_gen - a_gen) > 0) ? 1 : 0;
}

/* 1 つのスロットを読む。*valid = 有効か。ファイルが無い・長さが違う・不正 = 無効(戻り値 OK)。I/O エラーは負。 */
static int read_slot(const iso_fs_t *fs, const char *path, bool *valid, iso_meta_t *m) {
    *valid = false;
    iso_stat_t st;
    int rc = fs->stat(fs->ctx, path, &st);
    if (rc == ISO_ERR_NOT_FOUND) return ISO_OK;
    if (rc < 0) return rc;
    if (st.is_dir || st.size != ISO_META_SIZE) return ISO_OK;
    iso_file_t *f = NULL;
    rc = fs->open_for_read(fs->ctx, path, &f);
    if (rc == ISO_ERR_NOT_FOUND) return ISO_OK;
    if (rc < 0) return rc;
    uint8_t buf[ISO_META_SIZE];
    size_t got = 0;
    rc = fs->read(fs->ctx, f, buf, sizeof(buf), &got);
    fs->close(fs->ctx, f);
    if (rc < 0) return rc;
    if (got != ISO_META_SIZE) return ISO_OK;
    *valid = (iso_meta_decode(buf, sizeof(buf), m) == ISO_OK);
    return ISO_OK;
}

int iso_slots_read(const iso_fs_t *fs, const char *id, iso_slots_t *out) {
    if (!fs || !out || !iso_id_valid(id)) return ISO_ERR_INVALID;
    memset(out, 0, sizeof(*out));
    for (int s = 0; s < 2; s++) {
        char path[ISO_PATH_MAX];
        int rc = path_slot(path, sizeof(path), id, s);
        if (rc < 0) return rc;
        rc = read_slot(fs, path, &out->valid[s], &out->meta[s]);
        if (rc < 0) return rc;
    }
    out->adopted = (int8_t)iso_slot_choose(out->valid[0], out->meta[0].generation, out->valid[1], out->meta[1].generation);
    return ISO_OK;
}

static int write_slot(const iso_fs_t *fs, const char *id, int slot, const iso_meta_t *m) {
    uint8_t buf[ISO_META_SIZE];
    int rc = iso_meta_encode(m, buf);
    if (rc < 0) return rc;
    char path[ISO_PATH_MAX];
    rc = path_slot(path, sizeof(path), id, slot);
    if (rc < 0) return rc;
    return fs->overwrite(fs->ctx, path, buf, sizeof(buf));
}

/* ---- カタログ ------------------------------------------------------------- */

void iso_catalog_init(iso_catalog_t *cat, const iso_fs_t *fs, const iso_random_t *rnd) {
    memset(cat, 0, sizeof(*cat));
    cat->fs = fs;
    cat->rnd = rnd;
}

static int mkdir_ok_if_exists(const iso_fs_t *fs, const char *path) {
    int rc = fs->mkdir(fs->ctx, path);
    return (rc == ISO_ERR_EXISTS) ? ISO_OK : rc;
}

int iso_catalog_ensure_dirs(const iso_catalog_t *cat) {
    int rc = mkdir_ok_if_exists(cat->fs, ROOT_DIR);
    if (rc < 0) return rc;
    return mkdir_ok_if_exists(cat->fs, IMPORT_DIR);
}

iso_catalog_entry_t *iso_catalog_find(iso_catalog_t *cat, const char *id) {
    if (!id) return NULL;
    for (size_t i = 0; i < cat->count; i++) {
        if (strcmp(cat->images[i].id, id) == 0) return &cat->images[i];
    }
    return NULL;
}

void iso_catalog_remove(iso_catalog_t *cat, const char *id) {
    for (size_t i = 0; i < cat->count; i++) {
        if (strcmp(cat->images[i].id, id) == 0) {
            for (size_t k = i + 1; k < cat->count; k++) cat->images[k - 1] = cat->images[k];
            cat->count--;
            memset(&cat->images[cat->count], 0, sizeof(cat->images[0]));
            return;
        }
    }
}

bool iso_catalog_name_exists(const iso_catalog_t *cat, const char *name, size_t len) {
    for (size_t i = 0; i < cat->count; i++) {
        const iso_catalog_entry_t *e = &cat->images[i];
        if (!e->meta_valid || e->state == ISO_STATE_DELETING) continue;
        if (iso_name_equal(e->meta.name, e->meta.name_len, name, len)) return true;
    }
    return false;
}

int iso_catalog_received(const iso_fs_t *fs, const char *id, const iso_meta_t *meta, uint64_t *out) {
    uint64_t total = 0;
    for (uint32_t i = 0; i < meta->part_count; i++) {
        uint64_t expect = iso_extent_part_bytes(meta->size_bytes, meta->part_blocks, i);
        if (expect == 0) break;
        char path[ISO_PATH_MAX];
        int rc = path_part(path, sizeof(path), id, i);
        if (rc < 0) return rc;
        iso_stat_t st;
        rc = fs->stat(fs->ctx, path, &st);
        if (rc == ISO_ERR_NOT_FOUND) break;
        if (rc < 0) return rc;
        if (st.is_dir) break;
        if (st.size >= expect) {
            total += expect;
        } else {
            total += st.size;
            break;
        }
    }
    *out = total;
    return ISO_OK;
}

/* 1 つのイメージのディレクトリを読んで、エントリを作る */
static int load_entry(iso_catalog_t *cat, const char *id, iso_catalog_entry_t *e) {
    iso_slots_t sl;
    int rc = iso_slots_read(cat->fs, id, &sl);
    if (rc < 0) return rc;
    memset(e, 0, sizeof(*e));
    memcpy(e->id, id, ISO_ID_LEN);
    e->adopted = sl.adopted;
    if (sl.adopted < 0) {
        e->meta_valid = false;
        e->state = ISO_STATE_INVALID;
        e->invalid_reason = ISO_INVALID_META_CORRUPT;
        return ISO_OK;
    }
    e->meta_valid = true;
    e->meta = sl.meta[sl.adopted];
    iso_state_t persisted = iso_state_from_meta(e->meta.state);
    (void)iso_state_transition(persisted, ISO_EV_REBOOT, NULL, &e->state);
    e->resume_delete = (e->meta.state == ISO_META_DELETING);
    if (e->state == ISO_STATE_INVALID) e->invalid_reason = e->meta.invalid_reason;
    switch (e->state) {
    case ISO_STATE_READY:
        e->received = e->meta.size_bytes;
        break;
    case ISO_STATE_INCOMPLETE:
    case ISO_STATE_INVALID:
        rc = iso_catalog_received(cat->fs, id, &e->meta, &e->received);
        if (rc < 0) return rc;
        break;
    default:
        break;
    }
    return ISO_OK;
}

int iso_catalog_scan(iso_catalog_t *cat) {
    const iso_fs_t *fs = cat->fs;
    cat->count = 0;
    cat->hidden = 0;
    memset(cat->images, 0, sizeof(cat->images));
    int rc = iso_catalog_ensure_dirs(cat);
    if (rc < 0) return rc;

    /* 列挙を先に終えて、閉じてから、各ディレクトリを読む */
    char ids[ISO_MAX_IMAGES][ISO_ID_LEN + 1];
    size_t n = 0;
    iso_dir_t *dir = NULL;
    rc = fs->dir_open(fs->ctx, ROOT_DIR, &dir);
    if (rc < 0) return rc;
    for (;;) {
        char name[ISO_NAME_MAX + 1];
        iso_stat_t st;
        memset(&st, 0, sizeof(st));
        rc = fs->dir_next(fs->ctx, dir, name, &st);
        if (rc < 0) {
            fs->dir_close(fs->ctx, dir);
            return rc;
        }
        if (rc == 1) break;
        if (!st.is_dir || !iso_id_valid(name)) continue;
        if (n < ISO_MAX_IMAGES) {
            memcpy(ids[n], name, ISO_ID_LEN + 1);
            n++;
        } else {
            cat->hidden++;
        }
    }
    fs->dir_close(fs->ctx, dir);

    for (size_t i = 0; i < n; i++) {
        rc = load_entry(cat, ids[i], &cat->images[cat->count]);
        if (rc < 0) {
            memset(&cat->images[cat->count], 0, sizeof(cat->images[0]));
            return rc;
        }
        cat->count++;
    }
    return ISO_OK;
}

/* ---- begin ---------------------------------------------------------------- */

int iso_id_generate(const iso_fs_t *fs, const iso_random_t *rnd, char out[ISO_ID_LEN + 1]) {
    for (unsigned tries = 0; tries < ISO_ID_TRIES; tries++) {
        unsigned v = (unsigned)rnd->next(rnd->ctx);
        snprintf(out, ISO_ID_LEN + 1, "%08x", v);
        char path[ISO_PATH_MAX];
        int rc = path_dir(path, sizeof(path), out);
        if (rc < 0) return rc;
        rc = fs->mkdir(fs->ctx, path);
        if (rc == ISO_OK) return ISO_OK;
        if (rc != ISO_ERR_EXISTS) {
            out[0] = '\0';
            return rc;
        }
    }
    out[0] = '\0';
    return ISO_CAT_ERR_ID_EXHAUSTED;
}

/* 作りかけのディレクトリの片づけ(ベストエフォート。エラーは見ない) */
static void cleanup_new_dir(const iso_fs_t *fs, const char *id) {
    char path[ISO_PATH_MAX];
    if (path_slot(path, sizeof(path), id, 0) == ISO_OK) (void)fs->remove(fs->ctx, path);
    if (path_dir(path, sizeof(path), id) == ISO_OK) (void)fs->remove(fs->ctx, path);
}

int iso_catalog_begin(iso_catalog_t *cat, const char *name, size_t name_len, uint64_t size_bytes, const uint8_t *sha256,
                      char out_id[ISO_ID_LEN + 1]) {
    if (!name || !out_id) return ISO_ERR_INVALID;
    if (!iso_name_valid(name, name_len)) return ISO_ERR_INVALID;
    if (size_bytes < ISO_IMAGE_BYTES_MIN || size_bytes > ISO_IMAGE_BYTES_MAX) return ISO_ERR_INVALID;
    if (iso_catalog_name_exists(cat, name, name_len)) return ISO_CAT_ERR_NAME_EXISTS;
    if (cat->count >= ISO_MAX_IMAGES) return ISO_CAT_ERR_TOO_MANY;

    char id[ISO_ID_LEN + 1];
    int rc = iso_id_generate(cat->fs, cat->rnd, id);
    if (rc < 0) return rc;

    iso_meta_t m;
    memset(&m, 0, sizeof(m));
    m.state = ISO_META_UPLOADING;
    m.size_bytes = size_bytes;
    m.part_blocks = ISO_PART_BLOCKS_DEFAULT;
    m.part_count = (uint16_t)iso_extent_part_count(size_bytes, ISO_PART_BLOCKS_DEFAULT);
    if (sha256) {
        memcpy(m.sha256, sha256, ISO_SHA256_LEN);
        m.sha_flag = ISO_SHA_DECLARED;
    }
    m.name_len = (uint8_t)name_len;
    memcpy(m.name, name, name_len);
    m.generation = 1;
    rc = write_slot(cat->fs, id, 0, &m);
    if (rc < 0) {
        cleanup_new_dir(cat->fs, id);
        return rc;
    }

    iso_catalog_entry_t *e = &cat->images[cat->count++];
    memset(e, 0, sizeof(*e));
    memcpy(e->id, id, ISO_ID_LEN + 1);
    e->state = ISO_STATE_UPLOADING;
    e->meta_valid = true;
    e->adopted = 0;
    e->meta = m;
    memcpy(out_id, id, ISO_ID_LEN + 1);
    return ISO_OK;
}

/* ---- meta の更新、イベントの適用 ------------------------------------------- */

int iso_catalog_write_meta(iso_catalog_t *cat, iso_catalog_entry_t *e, const iso_meta_t *next) {
    if (!e->meta_valid) return ISO_ERR_INVALID;
    iso_meta_t m = *next;
    m.generation = e->meta.generation + 1u; /* UINT32_MAX の次は 0 (比較が巻き戻りを許す) */
    int target = (e->adopted == 0) ? 1 : 0;
    int rc = write_slot(cat->fs, e->id, target, &m);
    if (rc < 0) return rc;
    e->meta = m;
    e->adopted = (int8_t)target;
    return ISO_OK;
}

int iso_catalog_apply(iso_catalog_t *cat, iso_catalog_entry_t *e, iso_event_t ev, const iso_trans_ctx_t *ctx,
                      const iso_apply_arg_t *arg) {
    iso_trans_ctx_t c = {false, false, false};
    if (ctx) c = *ctx;
    c.received_complete = e->meta_valid && e->received == e->meta.size_bytes;

    iso_state_t next;
    iso_trans_t tr = iso_state_transition(e->state, ev, &c, &next);
    if (tr != ISO_TR_OK) return (int)tr;

    bool writes = (ev == ISO_EV_IDLE_TIMEOUT || ev == ISO_EV_WRITE_FAIL || ev == ISO_EV_FINALIZE_OK ||
                   ev == ISO_EV_FINALIZE_FAIL || ev == ISO_EV_FINALIZE_IOERR || ev == ISO_EV_INDEX_FAIL);
    /* INVALID のまま変わらないイベント(REBOOT など)では、理由(meta_corrupt を含む)を保つ。INVALID に入るイベントは、引数の理由 */
    uint8_t reason = (next == ISO_STATE_INVALID && e->state == ISO_STATE_INVALID) ? e->invalid_reason : 0;
    if (writes) {
        if (!e->meta_valid) return ISO_ERR_INVALID;
        iso_meta_t m = e->meta;
        int ms = iso_state_to_meta(next);
        m.state = (uint8_t)ms;
        if (next == ISO_STATE_INVALID) {
            if (!arg || arg->invalid_reason < ISO_INVALID_CHECKSUM_MISMATCH || arg->invalid_reason > ISO_INVALID_PART_MISMATCH)
                return ISO_ERR_INVALID;
            reason = arg->invalid_reason;
        }
        m.invalid_reason = reason;
        if (ev == ISO_EV_FINALIZE_OK && arg && arg->set_sha) {
            if (arg->sha_flag == ISO_SHA_COMPUTED) {
                if (!arg->sha256) return ISO_ERR_INVALID;
                memcpy(m.sha256, arg->sha256, ISO_SHA256_LEN);
                m.sha_flag = ISO_SHA_COMPUTED;
            } else if (arg->sha_flag == ISO_SHA_NONE) {
                memset(m.sha256, 0, ISO_SHA256_LEN);
                m.sha_flag = ISO_SHA_NONE;
            } else {
                return ISO_ERR_INVALID;
            }
        }
        int rc = iso_catalog_write_meta(cat, e, &m);
        if (rc < 0) return rc;
    }

    if (next == ISO_STATE_NONE) {
        iso_catalog_remove(cat, e->id); /* e は無効になる */
        return ISO_OK;
    }
    e->state = next;
    if (next == ISO_STATE_INVALID) {
        e->invalid_reason = reason;
    } else {
        e->invalid_reason = 0;
    }
    if (ev == ISO_EV_DELETE) e->resume_delete = false;
    return ISO_OK;
}

/* ---- 削除 ----------------------------------------------------------------- */

enum { PH_INIT = 0, PH_FILES, PH_SLOT_OLD, PH_SLOT_NEW, PH_RMDIR, PH_DONE };

int iso_delete_job_init(iso_delete_job_t *job, const char *id) {
    if (!job || !iso_id_valid(id)) return ISO_ERR_INVALID;
    memset(job, 0, sizeof(*job));
    memcpy(job->id, id, ISO_ID_LEN + 1);
    job->phase = PH_INIT;
    job->keep = -1;
    return ISO_OK;
}

static iso_step_result_t step_continue(void) {
    iso_step_result_t r = {ISO_STEP_CONTINUE, ISO_OK};
    return r;
}
static iso_step_result_t step_done(void) {
    iso_step_result_t r = {ISO_STEP_DONE, ISO_OK};
    return r;
}
static iso_step_result_t step_error(int err) {
    iso_step_result_t r = {ISO_STEP_ERROR, err};
    return r;
}

/* remove。すでに無ければ OK */
static int remove_if_exists(const iso_fs_t *fs, const char *path) {
    int rc = fs->remove(fs->ctx, path);
    return (rc == ISO_ERR_NOT_FOUND) ? ISO_OK : rc;
}

/* <id> のディレクトリで、名前が 'p' で始まる最初のエントリを探す。*found = 見つかったか。 */
static int find_part_entry(const iso_fs_t *fs, const char *id, char *name_out, bool *found) {
    *found = false;
    char dpath[ISO_PATH_MAX];
    int rc = path_dir(dpath, sizeof(dpath), id);
    if (rc < 0) return rc;
    iso_dir_t *dir = NULL;
    rc = fs->dir_open(fs->ctx, dpath, &dir);
    if (rc == ISO_ERR_NOT_FOUND) return ISO_OK;
    if (rc < 0) return rc;
    for (;;) {
        char name[ISO_NAME_MAX + 1];
        iso_stat_t st;
        memset(&st, 0, sizeof(st));
        rc = fs->dir_next(fs->ctx, dir, name, &st);
        if (rc < 0) {
            fs->dir_close(fs->ctx, dir);
            return rc;
        }
        if (rc == 1) break;
        if (name[0] == 'p' && !st.is_dir) { /* ファイルだけ(p で始まるディレクトリは、外来のものとして残す) */
            memcpy(name_out, name, sizeof(name));
            *found = true;
            break;
        }
    }
    fs->dir_close(fs->ctx, dir);
    return ISO_OK;
}

/* <id>/p000 の先頭クラスタが、/iso/import/ のファイルと同じか */
static int find_crosslink(const iso_fs_t *fs, const char *id, bool *out) {
    *out = false;
    char path[ISO_PATH_MAX];
    int rc = path_part(path, sizeof(path), id, 0);
    if (rc < 0) return rc;
    iso_stat_t p0;
    rc = fs->stat(fs->ctx, path, &p0);
    if (rc == ISO_ERR_NOT_FOUND) return ISO_OK;
    if (rc < 0) return rc;
    if (p0.is_dir || p0.first_cluster == 0) return ISO_OK;
    iso_dir_t *dir = NULL;
    rc = fs->dir_open(fs->ctx, IMPORT_DIR, &dir);
    if (rc == ISO_ERR_NOT_FOUND) return ISO_OK;
    if (rc < 0) return rc;
    for (;;) {
        char name[ISO_NAME_MAX + 1];
        iso_stat_t st;
        memset(&st, 0, sizeof(st));
        rc = fs->dir_next(fs->ctx, dir, name, &st);
        if (rc < 0) {
            fs->dir_close(fs->ctx, dir);
            return rc;
        }
        if (rc == 1) break;
        if (!st.is_dir && st.first_cluster == p0.first_cluster) {
            *out = true;
            break;
        }
    }
    fs->dir_close(fs->ctx, dir);
    return ISO_OK;
}

int iso_catalog_delete_precheck(const iso_catalog_t *cat, const char *id) {
    if (!cat || !iso_id_valid(id)) return ISO_ERR_INVALID;
    bool cross = false;
    int rc = find_crosslink(cat->fs, id, &cross);
    if (rc < 0) return rc;
    return cross ? ISO_CAT_ERR_CROSSLINKED : ISO_OK;
}

iso_step_result_t iso_catalog_delete_step(iso_catalog_t *cat, iso_delete_job_t *job) {
    const iso_fs_t *fs = cat->fs;
    for (;;) {
        char path[ISO_PATH_MAX];
        int rc;
        switch (job->phase) {
        case PH_INIT: {
            /* 何かを変更する前に、クロスリンクを照合する(再開でも同じ。一致したら、何も消さずに止める) */
            bool cross = false;
            rc = find_crosslink(fs, job->id, &cross);
            if (rc < 0) return step_error(rc);
            if (cross) return step_error(ISO_CAT_ERR_CROSSLINKED);
            iso_slots_t sl;
            rc = iso_slots_read(fs, job->id, &sl);
            if (rc < 0) return step_error(rc);
            job->keep = sl.adopted;
            iso_catalog_entry_t *e = iso_catalog_find(cat, job->id);
            if (sl.adopted >= 0 && sl.meta[sl.adopted].state != ISO_META_DELETING) {
                iso_meta_t m = sl.meta[sl.adopted];
                m.state = ISO_META_DELETING;
                m.generation = m.generation + 1u;
                int target = 1 - sl.adopted;
                rc = write_slot(fs, job->id, target, &m);
                if (rc < 0) return step_error(rc);
                job->keep = (int8_t)target;
                if (e && e->meta_valid) {
                    e->meta = m;
                    e->adopted = (int8_t)target;
                }
            }
            if (e) e->state = ISO_STATE_DELETING;
            job->phase = PH_FILES;
            return step_continue();
        }
        case PH_FILES: {
            char name[ISO_NAME_MAX + 1];
            bool found = false;
            rc = find_part_entry(fs, job->id, name, &found);
            if (rc < 0) return step_error(rc);
            if (!found) {
                job->phase = PH_SLOT_OLD;
                continue;
            }
            int n = snprintf(path, sizeof(path), "/iso/%s/%s", job->id, name);
            if (n < 0 || (size_t)n >= sizeof(path)) return step_error(ISO_ERR_INVALID);
            rc = remove_if_exists(fs, path);
            if (rc < 0) return step_error(rc);
            return step_continue();
        }
        case PH_SLOT_OLD: {
            int old_slot = (job->keep < 0) ? 0 : 1 - job->keep;
            rc = path_slot(path, sizeof(path), job->id, old_slot);
            if (rc < 0) return step_error(rc);
            rc = remove_if_exists(fs, path);
            if (rc < 0) return step_error(rc);
            job->phase = PH_SLOT_NEW;
            return step_continue();
        }
        case PH_SLOT_NEW: {
            int new_slot = (job->keep < 0) ? 1 : job->keep;
            rc = path_slot(path, sizeof(path), job->id, new_slot);
            if (rc < 0) return step_error(rc);
            rc = remove_if_exists(fs, path);
            if (rc < 0) return step_error(rc);
            job->phase = PH_RMDIR;
            return step_continue();
        }
        case PH_RMDIR:
            rc = path_dir(path, sizeof(path), job->id);
            if (rc < 0) return step_error(rc);
            rc = remove_if_exists(fs, path);
            if (rc < 0) return step_error(rc);
            job->phase = PH_DONE;
            iso_catalog_remove(cat, job->id);
            return step_done();
        default:
            return step_done();
        }
    }
}

/* ---- import --------------------------------------------------------------- */

#define SIG_OFFSET (16u * ISO_CD_BLOCK_BYTES)

/* 拡張子 .iso (大文字小文字を区別しない) */
static bool has_iso_ext(const char *name, size_t len) {
    if (len < 4) return false;
    const char *e = name + len - 4;
    return e[0] == '.' && (e[1] == 'i' || e[1] == 'I') && (e[2] == 's' || e[2] == 'S') && (e[3] == 'o' || e[3] == 'O');
}

/* セクタ 16 (CD のブロック 16) のバイト 1〜5 が CD001 または BEA01 か。読み出しは先頭から順(seam に seek がない)。 */
static int check_signature(const iso_fs_t *fs, const char *path, bool *ok) {
    *ok = false;
    iso_file_t *f = NULL;
    int rc = fs->open_for_read(fs->ctx, path, &f);
    if (rc < 0) return rc;
    uint8_t buf[512];
    size_t remaining = SIG_OFFSET;
    while (remaining > 0) {
        size_t want = remaining < sizeof(buf) ? remaining : sizeof(buf);
        size_t got = 0;
        rc = fs->read(fs->ctx, f, buf, want, &got);
        if (rc < 0 || got != want) {
            fs->close(fs->ctx, f);
            return rc < 0 ? rc : ISO_OK; /* 短い = ISO ではない */
        }
        remaining -= want;
    }
    size_t got = 0;
    rc = fs->read(fs->ctx, f, buf, 6, &got);
    fs->close(fs->ctx, f);
    if (rc < 0) return rc;
    if (got == 6 && (memcmp(buf + 1, "CD001", 5) == 0 || memcmp(buf + 1, "BEA01", 5) == 0)) *ok = true;
    return ISO_OK;
}

/* 先頭クラスタが、既存のどれかの <id>/p000 と同じか */
static int is_crosslinked(const iso_catalog_t *cat, uint32_t first_cluster, bool *out) {
    *out = false;
    if (first_cluster == 0) return ISO_OK;
    for (size_t i = 0; i < cat->count; i++) {
        char path[ISO_PATH_MAX];
        int rc = path_part(path, sizeof(path), cat->images[i].id, 0);
        if (rc < 0) return rc;
        iso_stat_t st;
        rc = cat->fs->stat(cat->fs->ctx, path, &st);
        if (rc == ISO_ERR_NOT_FOUND) continue;
        if (rc < 0) return rc;
        if (!st.is_dir && st.first_cluster == first_cluster) {
            *out = true;
            return ISO_OK;
        }
    }
    return ISO_OK;
}

/* 1 つのファイルを調べて、取り込む。*imported = 取り込んだか(false = skipped)。エラーは I/O のものだけ。 */
static int import_one(iso_catalog_t *cat, const char *fname, const iso_stat_t *st, bool *imported) {
    const iso_fs_t *fs = cat->fs;
    *imported = false;
    size_t len = strlen(fname);
    size_t base = len - 4;
    if (!iso_name_valid(fname, base)) return ISO_OK; /* 空、63 バイト超、不正な UTF-8 などを含む */
    if (st->size < ISO_IMAGE_BYTES_MIN) return ISO_OK;
    uint64_t blocks = iso_extent_capacity_blocks(st->size);
    if (blocks > ISO_PART_BLOCKS_MAX) return ISO_OK;
    if (iso_catalog_name_exists(cat, fname, base)) return ISO_OK;
    if (cat->count >= ISO_MAX_IMAGES) return ISO_OK;

    char src[ISO_PATH_MAX];
    int n = snprintf(src, sizeof(src), IMPORT_DIR "/%s", fname);
    if (n < 0 || (size_t)n >= sizeof(src)) return ISO_OK;

    bool cross = false;
    int rc = is_crosslinked(cat, st->first_cluster, &cross);
    if (rc < 0) return rc;
    if (cross) return ISO_OK;

    bool sig = false;
    rc = check_signature(fs, src, &sig);
    if (rc < 0) return rc;
    if (!sig) return ISO_OK;

    char id[ISO_ID_LEN + 1];
    rc = iso_id_generate(fs, cat->rnd, id);
    if (rc < 0) return rc;

    iso_meta_t m;
    memset(&m, 0, sizeof(m));
    m.state = ISO_META_READY;
    m.size_bytes = st->size;
    m.part_count = 1;
    m.part_blocks = (uint32_t)blocks;
    m.sha_flag = ISO_SHA_NONE;
    m.name_len = (uint8_t)base;
    memcpy(m.name, fname, base);
    m.generation = 1;
    rc = write_slot(fs, id, 0, &m);
    if (rc < 0) {
        cleanup_new_dir(fs, id);
        return rc;
    }
    char dst[ISO_PATH_MAX];
    rc = path_part(dst, sizeof(dst), id, 0);
    if (rc == ISO_OK) rc = fs->rename(fs->ctx, src, dst);
    if (rc < 0) {
        cleanup_new_dir(fs, id);
        return rc;
    }

    iso_catalog_entry_t *e = &cat->images[cat->count++];
    memset(e, 0, sizeof(*e));
    memcpy(e->id, id, ISO_ID_LEN + 1);
    e->state = ISO_STATE_READY;
    e->meta_valid = true;
    e->adopted = 0;
    e->received = m.size_bytes;
    e->meta = m;
    *imported = true;
    return ISO_OK;
}

typedef struct {
    char name[ISO_NAME_MAX + 1];
    iso_stat_t st;
} import_entry_t;

int iso_catalog_rescan(iso_catalog_t *cat, uint32_t cursor, iso_rescan_result_t *out) {
    const iso_fs_t *fs = cat->fs;
    memset(out, 0, sizeof(*out));

    import_entry_t batch[ISO_RESCAN_BATCH];
    size_t nb = 0;
    bool more = false;
    iso_dir_t *dir = NULL;
    int rc = fs->dir_open(fs->ctx, IMPORT_DIR, &dir);
    if (rc == ISO_ERR_NOT_FOUND) return ISO_OK;
    if (rc < 0) return rc;
    uint32_t seen = 0;
    for (;;) {
        char name[ISO_NAME_MAX + 1];
        iso_stat_t st;
        memset(&st, 0, sizeof(st));
        rc = fs->dir_next(fs->ctx, dir, name, &st);
        if (rc < 0) {
            fs->dir_close(fs->ctx, dir);
            return rc;
        }
        if (rc == 1) break;
        if (st.is_dir || !has_iso_ext(name, strlen(name))) continue;
        if (seen < cursor) {
            seen++;
            continue;
        }
        if (nb == ISO_RESCAN_BATCH) {
            more = true;
            break;
        }
        memcpy(batch[nb].name, name, sizeof(name));
        batch[nb].st = st;
        nb++;
    }
    fs->dir_close(fs->ctx, dir);

    for (size_t i = 0; i < nb; i++) {
        bool imported = false;
        rc = import_one(cat, batch[i].name, &batch[i].st, &imported);
        if (rc < 0) return rc;
        if (imported) {
            out->imported++;
        } else {
            out->skipped++;
        }
    }
    if (more) {
        out->has_next = true;
        out->next_cursor = cursor + out->skipped;
    }
    return ISO_OK;
}
