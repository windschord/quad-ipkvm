/*
 * iso_core の固定シードのファズ: ランダムな操作列(PUT、begin、submit(FINISH / DELETE / IDLE_CLOSE / INDEX)、ワーカーのステップ(途中までで
 * 止める。他の操作と交わる)、tick(時刻の飛びと巻き戻り)、MSC の READ の参照の取得と解放、エラー・書き込みの失敗の注入、電源断、SD の再初期化、起動)。
 * 操作のたびに、不変条件を調べる:
 *   - キュー: 長さ <= 8、同じイメージのジョブは 1 つ(動いているジョブと待っている DELETE / FINISH の組だけ例外)、INDEX の集合に重複がない。
 *   - エクステントの表: カタログの ready(または削除待ちの deleting)のイメージにだけあり、重複せず、参照 >= 1。
 *   - 枠: uploading / finalizing は 1 つ以下、iso_upload_t の枠と一致、ハンドルは枠の持ち主のものだけ。
 *   - ディスク: パーツの中身 = ソースの先頭(実装を使わない基準)、ready のイメージの SHA-256 = ソースから別に計算した値、
 *     invalid の理由は、宣言値の食い違い(checksum_mismatch)と署名(not_iso)だけ(電源断や I/O の失敗で part_mismatch にならない)。
 *   - ビュー: iso_core_publish の後のビューが、カタログと表の状態と一致する(ready で表がなければ finalizing の表示)。
 * 最後に、注入と電源断を止め、参照を放して全部のジョブを流し切り、静止した状態を確かめる(ready は全部、表がある。deleting は残らない)。
 */
#include "core_common.h"

void setUp(void) {}
void tearDown(void) {}

typedef struct {
    uint64_t s;
} rng_t;
static uint64_t rnd_next(rng_t *r) {
    r->s ^= r->s << 13;
    r->s ^= r->s >> 7;
    r->s ^= r->s << 17;
    return r->s;
}
static uint32_t rnd(rng_t *r, uint32_t n) { return (uint32_t)(rnd_next(r) % n); }

static rng_t R;
static uint32_t g_chunk, g_sync;
static bool g_bad_sig;
static bool g_inject; /* いまの操作に、注入した */

#define MAX_READERS 4
static iso_extent_table_t *READERS[MAX_READERS];

static int st_ops, st_ready, st_invalid, st_deleted, st_cuts, st_crash, st_reinit, st_finish_err, st_delete_busy, st_joined, st_overflow, st_readers;

static iso_upload_t *up(void) { return iso_core_upload(&CORE); }

/* ---- 基準による照合 ------------------------------------------------------------------------ */

typedef struct {
    uint64_t mem;
    uint64_t dir;
} chain_t;

static chain_t check_chain(const iso_catalog_entry_t *e) {
    const uint64_t pbytes = (uint64_t)e->meta.part_blocks * 2048ull;
    chain_t c = {0, 0};
    bool prev_full = true, missing = false;
    for (unsigned i = 0; i < ISO_MAX_PARTS; i++) {
        char path[ISO_PATH_MAX];
        part_path(path, sizeof(path), e->id, i);
        int64_t ms = mock_fs_mem_size(&FS, path);
        if (ms < 0) {
            missing = true;
            continue; /* 後ろのパーツが空でないのは、抜けがあるということ */
        }
        TEST_ASSERT_TRUE_MESSAGE(!missing || ms == 0, "a non-empty part follows a missing part");
        int64_t ds = mock_fs_size(&FS, path);
        TEST_ASSERT_TRUE((uint64_t)ms <= pbytes);
        TEST_ASSERT_TRUE(ds <= ms);
        if (ms > 0) TEST_ASSERT_TRUE_MESSAGE(prev_full, "a non-empty part follows a part that is not full");
        size_t dl = 0;
        const uint8_t *d = mock_fs_data(&FS, path, &dl);
        for (int64_t k = 0; k < ms; k++) {
            if (!d || (size_t)k >= dl || d[k] != gen_byte(c.mem + (uint64_t)k)) TEST_FAIL_MESSAGE("part content differs from the source");
        }
        c.mem += (uint64_t)ms;
        c.dir += (uint64_t)ds;
        prev_full = ((uint64_t)ms == pbytes);
    }
    return c;
}

static void check_queue(void) {
    const iso_core_t *c = &CORE;
    TEST_ASSERT_TRUE(c->qn <= ISO_WORKER_QUEUE);
    for (size_t i = 0; i < c->qn; i++) {
        TEST_ASSERT_TRUE(c->queue[i].kind >= ISO_JOB_DELETE && c->queue[i].kind <= ISO_JOB_IDLE_CLOSE);
        TEST_ASSERT_NOT_NULL(iso_catalog_find(&CAT, c->queue[i].id));
        for (size_t k = i + 1; k < c->qn; k++) TEST_ASSERT_TRUE_MESSAGE(strcmp(c->queue[i].id, c->queue[k].id) != 0, "two queued jobs for one image");
        if (c->cur_active && strcmp(c->cur.id, c->queue[i].id) == 0) {
            bool ok = (c->queue[i].kind == ISO_JOB_DELETE && c->cur.kind != ISO_JOB_DELETE) ||
                      (c->queue[i].kind == ISO_JOB_FINISH && c->cur.kind == ISO_JOB_IDLE_CLOSE);
            TEST_ASSERT_TRUE_MESSAGE(ok, "a queued job duplicates the running job");
        }
    }
    for (size_t i = 0; i < c->index_n; i++) {
        TEST_ASSERT_NOT_NULL(iso_catalog_find(&CAT, c->index_ids[i]));
        for (size_t k = i + 1; k < c->index_n; k++) TEST_ASSERT_TRUE(strcmp(c->index_ids[i], c->index_ids[k]) != 0);
        for (size_t k = 0; k < c->qn; k++) TEST_ASSERT_TRUE(strcmp(c->index_ids[i], c->queue[k].id) != 0 || c->queue[k].kind == ISO_JOB_IDLE_CLOSE);
    }
    for (size_t i = 0; i < c->table_n; i++) {
        const iso_catalog_entry_t *e = iso_catalog_find(&CAT, c->tables[i].id);
        TEST_ASSERT_NOT_NULL(e);
        TEST_ASSERT_TRUE(e->state == ISO_STATE_READY || e->state == ISO_STATE_DELETING);
        TEST_ASSERT_NOT_NULL(c->tables[i].table);
        TEST_ASSERT_TRUE(iso_extent_table_refs(c->tables[i].table) >= 1);
        for (size_t k = i + 1; k < c->table_n; k++) TEST_ASSERT_TRUE(strcmp(c->tables[i].id, c->tables[k].id) != 0);
    }
}

static void check_images(void) {
    int active = 0;
    for (size_t i = 0; i < CAT.count; i++) {
        const iso_catalog_entry_t *e = &CAT.images[i];
        if (e->state == ISO_STATE_DELETING) continue;
        if (!e->meta_valid) continue;
        iso_slots_t s;
        TEST_ASSERT_EQUAL_INT(ISO_OK, iso_slots_read(&IFS, e->id, &s));
        TEST_ASSERT_TRUE(s.adopted >= 0);
        switch (e->state) {
        case ISO_STATE_UPLOADING: {
            active++;
            chain_t c = check_chain(e);
            TEST_ASSERT_EQUAL_UINT64(c.mem, e->received);
            TEST_ASSERT_TRUE(up()->slot_active && !up()->slot_finalizing && strcmp(up()->slot_id, e->id) == 0);
            break;
        }
        case ISO_STATE_FINALIZING:
            active++;
            TEST_ASSERT_TRUE(up()->slot_active && up()->slot_finalizing && strcmp(up()->slot_id, e->id) == 0);
            break;
        case ISO_STATE_INCOMPLETE: {
            chain_t c = check_chain(e);
            TEST_ASSERT_TRUE(e->received <= c.mem);
            TEST_ASSERT_TRUE(s.meta[s.adopted].state == ISO_META_UPLOADING || s.meta[s.adopted].state == ISO_META_INCOMPLETE);
            break;
        }
        case ISO_STATE_READY: {
            TEST_ASSERT_EQUAL_UINT8(ISO_META_READY, s.meta[s.adopted].state);
            TEST_ASSERT_EQUAL_UINT8(ISO_SHA_COMPUTED, e->meta.sha_flag);
            uint8_t want[32];
            gen_sha(e->meta.size_bytes, want);
            TEST_ASSERT_EQUAL_HEX8_ARRAY(want, e->meta.sha256, 32);
            chain_t c = check_chain(e);
            TEST_ASSERT_EQUAL_UINT64(e->meta.size_bytes, c.dir);
            break;
        }
        case ISO_STATE_INVALID: {
            /* 理由は、宣言値の食い違いと署名だけ(電源断・I/O の失敗で、part_mismatch / too_fragmented にならない) */
            if (e->invalid_reason == ISO_INVALID_CHECKSUM_MISMATCH) {
                uint8_t want[32];
                gen_sha(e->meta.size_bytes, want);
                TEST_ASSERT_EQUAL_UINT8(ISO_SHA_DECLARED, e->meta.sha_flag);
                TEST_ASSERT_TRUE(memcmp(want, e->meta.sha256, 32) != 0);
            } else {
                TEST_ASSERT_EQUAL_UINT8(ISO_INVALID_NOT_ISO, e->invalid_reason);
                TEST_ASSERT_TRUE(g_bad_sig);
            }
            break;
        }
        default: break;
        }
    }
    TEST_ASSERT_TRUE(active <= 1);
    TEST_ASSERT_EQUAL_INT(active, up()->slot_active ? 1 : 0);
    TEST_ASSERT_EQUAL_INT(up()->f ? 1 : 0, FS.open_files);
    TEST_ASSERT_TRUE(!up()->f || up()->slot_active);
    TEST_ASSERT_EQUAL_INT(0, FS.open_dirs);
}

static void check_view(void) {
    iso_core_publish(&CORE);
    const iso_view_t *v = &VIEW_COPY;
    TEST_ASSERT_EQUAL_UINT(CAT.count, v->count);
    TEST_ASSERT_EQUAL_UINT32(CAT.hidden, v->hidden);
    TEST_ASSERT_EQUAL_INT(up()->slot_active, v->upload_active);
    for (size_t i = 0; i < CAT.count; i++) {
        const iso_catalog_entry_t *e = &CAT.images[i];
        const iso_view_image_t *w = &v->images[i];
        TEST_ASSERT_EQUAL_STRING(e->id, w->id);
        TEST_ASSERT_EQUAL_UINT64(e->received, w->received);
        TEST_ASSERT_EQUAL_INT(e->meta_valid, w->meta_valid);
        iso_extent_table_t *t = iso_core_table(&CORE, e->id);
        if (e->state == ISO_STATE_READY) {
            TEST_ASSERT_TRUE(w->table == t);
            TEST_ASSERT_EQUAL_INT(t ? ISO_STATE_READY : ISO_STATE_FINALIZING, w->state);
        } else {
            TEST_ASSERT_NULL(w->table);
            TEST_ASSERT_EQUAL_INT(e->state, w->state);
        }
        if (e->meta_valid) {
            TEST_ASSERT_EQUAL_UINT64(e->meta.size_bytes, w->size_bytes);
            TEST_ASSERT_EQUAL_UINT8(e->meta.name_len, w->name_len);
            TEST_ASSERT_EQUAL_STRING(e->meta.name, w->name);
        }
    }
}

static void check_all(void) {
    check_queue();
    check_images();
    if (rnd(&R, 4) == 0) check_view();
}

/* ---- 操作 ------------------------------------------------------------------------------- */

static void inject_for_next_op(void) {
    g_inject = false;
    uint32_t r = rnd(&R, 100);
    if (r < 10) {
        int nth = 1 + (int)rnd(&R, 3);
        uint32_t limit = rnd(&R, 6000);
        int err = rnd(&R, 2) ? ISO_ERR_IO : ISO_ERR_NO_SPACE;
        mock_fs_fail_write(&FS, nth, limit, err);
        g_inject = true;
    } else if (r < 18) {
        mock_fs_error_at(&FS, 1 + (int)rnd(&R, 6), ISO_ERR_IO);
        g_inject = true;
    }
}

static void clear_injection(void) {
    mock_fs_fail_write(&FS, 0, 0, 0);
    mock_fs_reset_counter(&FS);
    g_inject = false;
}

static iso_catalog_entry_t *pick_image(void) {
    if (!CAT.count) return NULL;
    return &CAT.images[rnd(&R, (uint32_t)CAT.count)];
}

static void op_begin(void) {
    if (iso_upload_slot_busy(up()) || CAT.count >= ISO_MAX_IMAGES - 2) return;
    static const uint32_t pbs[] = {PB_SMALL, 18, 25};
    uint32_t pb = pbs[rnd(&R, 3)];
    uint64_t size = 34816 + rnd(&R, 90000);
    char name[16];
    snprintf(name, sizeof(name), "n%u", (unsigned)rnd(&R, 1000000));
    uint8_t sha[32];
    const uint8_t *decl = NULL;
    uint32_t d = rnd(&R, 6);
    if (d == 0) {
        gen_sha(size, sha);
        decl = sha;
    } else if (d == 1) {
        gen_sha(size, sha);
        uint32_t byte = rnd(&R, 32);
        uint32_t bit = rnd(&R, 8);
        sha[byte] ^= (uint8_t)(1u << bit);
        decl = sha;
    }
    char id[9];
    int rc = iso_upload_begin(up(), name, strlen(name), size, decl, id);
    if (rc != ISO_OK) {
        TEST_ASSERT_TRUE(rc == ISO_RES_NAME_EXISTS || FS.dead || rc == ISO_ERR_IO || rc == ISO_RES_TOO_MANY_IMAGES);
        return;
    }
    iso_catalog_entry_t *e = iso_catalog_find(&CAT, id);
    iso_meta_t m = e->meta;
    m.part_blocks = pb;
    m.part_count = (uint16_t)iso_extent_part_count(size, pb);
    (void)iso_catalog_write_meta(&CAT, e, &m);
}

static void op_put(bool wrong_offset) {
    iso_catalog_entry_t *e = NULL;
    if (up()->slot_active && !up()->slot_finalizing) {
        e = iso_catalog_find(&CAT, up()->slot_id);
    } else {
        for (int tries = 0; tries < 4 && !e; tries++) {
            iso_catalog_entry_t *c = pick_image();
            if (c && c->state == ISO_STATE_INCOMPLETE && c->meta_valid) e = c;
        }
    }
    if (!e || !e->meta_valid) return;
    iso_upload_info_t info;
    iso_upload_info(up(), e->id, &info);
    if (!info.found || info.received >= info.size_bytes) return;
    uint64_t room = info.size_bytes - info.received;
    size_t len = 1 + rnd(&R, g_chunk);
    if (len > room) len = (size_t)room;
    uint64_t off = info.received;
    if (wrong_offset) off = (off + 1 + rnd(&R, 3000)) % (info.size_bytes - 1);
    else if (rnd(&R, 100) < 15) inject_for_next_op();
    iso_upload_write_t w;
    bool was_incomplete = (e->state == ISO_STATE_INCOMPLETE);
    int rc = put_range(e->id, off, len, &w);
    if (rc == ISO_RES_OFFSET_MISMATCH) {
        TEST_ASSERT_TRUE(wrong_offset || was_incomplete);
        if (!g_inject && !FS.dead && w.received < info.size_bytes) {
            size_t n = len;
            if (n > info.size_bytes - w.received) n = (size_t)(info.size_bytes - w.received);
            int rc2 = put_range(e->id, w.received, n, &w);
            TEST_ASSERT_TRUE(rc2 == ISO_OK || rc2 == ISO_RES_UPLOAD_IN_PROGRESS || FS.dead);
        }
    } else if (rc != ISO_OK) {
        TEST_ASSERT_TRUE_MESSAGE(g_inject || FS.dead || rc == ISO_RES_UPLOAD_IN_PROGRESS, "unexpected failure of a PUT");
    }
    clear_injection();
}

/* アップロードを送り切って、finish を投入する(完了までたどり着く経路を、十分な頻度で通す) */
static void op_complete(void) {
    iso_catalog_entry_t *e = NULL;
    if (up()->slot_active && !up()->slot_finalizing) {
        e = iso_catalog_find(&CAT, up()->slot_id);
    } else {
        for (int tries = 0; tries < 6 && !e; tries++) {
            iso_catalog_entry_t *c = pick_image();
            if (c && c->state == ISO_STATE_INCOMPLETE && c->meta_valid) e = c;
        }
    }
    if (!e || !e->meta_valid) return;
    char id[9];
    memcpy(id, e->id, 9);
    for (int guard = 0; guard < 200; guard++) {
        iso_upload_info_t info;
        iso_upload_info(up(), id, &info);
        if (!info.found || info.received >= info.size_bytes) break;
        uint64_t room = info.size_bytes - info.received;
        size_t len = 1 + rnd(&R, g_chunk);
        if (len > room) len = (size_t)room;
        iso_upload_write_t w;
        int rc = put_range(id, info.received, len, &w);
        if (rc == ISO_RES_OFFSET_MISMATCH) continue;
        if (rc != ISO_OK) break;
    }
    if (rnd(&R, 4) != 0) {
        uint64_t rec;
        (void)iso_core_submit_finish(&CORE, id, &rec);
    }
}

static void op_submit(void) {
    iso_catalog_entry_t *e = NULL;
    uint32_t r = rnd(&R, 100);
    if (r < 45 && up()->slot_active) {
        e = iso_catalog_find(&CAT, up()->slot_id);
    }
    if (!e) e = pick_image();
    if (!e) return;
    char id[9];
    memcpy(id, e->id, 9);
    uint32_t k = rnd(&R, 100);
    int rc;
    if (k < 40) {
        uint64_t rec = 0;
        rc = iso_core_submit_finish(&CORE, id, &rec);
        TEST_ASSERT_TRUE(rc == ISO_RES_OK || rc == ISO_RES_JOINED || rc == ISO_RES_ALREADY_READY || rc == ISO_RES_NOT_FOUND ||
                         rc == ISO_RES_INVALID_IMAGE || rc == ISO_RES_SIZE_MISMATCH || rc == ISO_RES_UPLOAD_IN_PROGRESS || rc == ISO_ERR_BUSY ||
                         rc < 0);
        if (rc == ISO_ERR_BUSY) st_overflow++;
        if (rc == ISO_RES_JOINED) st_joined++;
    } else if (k < 52) {
        rc = iso_core_submit_delete(&CORE, id);
        TEST_ASSERT_TRUE(rc == ISO_RES_OK || rc == ISO_RES_JOINED || rc == ISO_RES_NOT_FOUND || rc == ISO_ERR_BUSY);
        if (rc == ISO_ERR_BUSY) st_overflow++;
        if (rc == ISO_RES_JOINED) st_joined++;
        if (rc == ISO_RES_OK) {
            TEST_ASSERT_EQUAL_INT(ISO_STATE_DELETING, e->state);
            TEST_ASSERT_FALSE(up()->slot_active && strcmp(up()->slot_id, id) == 0);
        }
    } else if (k < 76) {
        rc = iso_core_submit(&CORE, ISO_JOB_IDLE_CLOSE, id);
        TEST_ASSERT_TRUE(rc == ISO_RES_OK || rc == ISO_RES_JOINED || rc == ISO_ERR_BUSY || rc == ISO_RES_NOT_FOUND);
    } else {
        rc = iso_core_submit(&CORE, ISO_JOB_INDEX, id);
        TEST_ASSERT_TRUE(rc == ISO_RES_OK || rc == ISO_RES_JOINED || rc == ISO_RES_NOT_UPLOADING || rc == ISO_RES_NOT_FOUND);
    }
}

static void op_worker(void) {
    int steps = 1 + (int)rnd(&R, 5);
    if (rnd(&R, 100) < 20) inject_for_next_op();
    for (int i = 0; i < steps; i++) {
        iso_job_t *j = iso_core_pick(&CORE);
        if (!j) break;
        iso_job_kind_t kind = j->kind;
        char id[9];
        memcpy(id, j->id, 9);
        iso_step_result_t r = iso_job_step(&CORE, j, rnd(&R, 3) ? 4096 : 0);
        if (r.kind == ISO_STEP_CONTINUE) {
            if (iso_job_waiting(j)) NOW_MS += 1000 * (1 + rnd(&R, 20)); /* 参照待ち: 時が進む */
            continue;
        }
        if (r.kind == ISO_STEP_ERROR) {
            if (kind == ISO_JOB_DELETE && r.err == ISO_ERR_BUSY) {
                st_delete_busy++;
            } else {
                TEST_ASSERT_TRUE_MESSAGE(g_inject || FS.dead, "a job failed without an injection");
                if (kind == ISO_JOB_FINISH) {
                    st_finish_err++;
                    const iso_catalog_entry_t *e = iso_catalog_find(&CAT, id);
                    if (e) TEST_ASSERT_TRUE(e->state == ISO_STATE_INCOMPLETE || e->state == ISO_STATE_INVALID || e->state == ISO_STATE_DELETING);
                }
            }
        } else if (kind == ISO_JOB_DELETE) {
            TEST_ASSERT_NULL(iso_catalog_find(&CAT, id));
            st_deleted++;
        } else if (kind == ISO_JOB_FINISH) {
            const iso_catalog_entry_t *e = iso_catalog_find(&CAT, id);
            if (e && e->state == ISO_STATE_READY) st_ready++;
            if (e && e->state == ISO_STATE_INVALID) st_invalid++;
            if (e) TEST_ASSERT_TRUE(e->state == ISO_STATE_READY || e->state == ISO_STATE_INVALID || e->state == ISO_STATE_DELETING);
        }
    }
    clear_injection();
}

static void op_tick(void) {
    uint32_t r = rnd(&R, 10);
    if (r < 2) NOW_MS += 301000;
    else if (r < 4) NOW_MS = (NOW_MS > 500000) ? NOW_MS - 400000 : NOW_MS; /* 巻き戻り */
    else NOW_MS += rnd(&R, 7000);
    iso_upload_tick(&CORE, NOW_MS);
}

static void op_reader(void) {
    int slot = (int)rnd(&R, MAX_READERS);
    if (READERS[slot]) {
        iso_extent_table_unref(READERS[slot]);
        READERS[slot] = NULL;
        return;
    }
    iso_catalog_entry_t *e = pick_image();
    if (!e) return;
    iso_extent_table_t *t = iso_core_table(&CORE, e->id);
    if (!t) return;
    iso_extent_table_ref(t);
    READERS[slot] = t;
    st_readers++;
}

static void reboot_after_power_loss(void) {
    mock_fs_power_on(&FS);
    mock_fs_reset_counter(&FS);
    mock_fs_fail_write(&FS, 0, 0, 0);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_core_boot(&CORE));
}

static void release_readers(void) {
    for (int i = 0; i < MAX_READERS; i++) {
        if (READERS[i]) iso_extent_table_unref(READERS[i]);
        READERS[i] = NULL;
    }
}

static void drain_and_check_quiescent(void) {
    mock_fs_power_on(&FS);
    clear_injection();
    release_readers();
    for (int round = 0; round < 6; round++) {
        iso_core_mark_unindexed(&CORE);
        /* deleting のまま、ジョブがないもの(あふれ、ERROR)は、再度の DELETE で続ける */
        for (size_t i = 0; i < CAT.count; i++) {
            if (CAT.images[i].state == ISO_STATE_DELETING) {
                char id[9];
                memcpy(id, CAT.images[i].id, 9);
                (void)iso_core_submit_delete(&CORE, id);
            }
        }
        /* 枠を使っているイメージは、完了させるか中止する */
        if (up()->slot_active && !up()->slot_finalizing) {
            char id[9];
            memcpy(id, up()->slot_id, 9);
            (void)iso_core_submit(&CORE, ISO_JOB_IDLE_CLOSE, id);
        }
        drive(4096, 1000);
    }
    for (size_t i = 0; i < CAT.count; i++) {
        const iso_catalog_entry_t *e = &CAT.images[i];
        TEST_ASSERT_TRUE_MESSAGE(e->state != ISO_STATE_DELETING, "a deleting image remains");
        TEST_ASSERT_TRUE_MESSAGE(e->state != ISO_STATE_FINALIZING && e->state != ISO_STATE_UPLOADING, "an active image remains");
        if (e->state == ISO_STATE_READY) TEST_ASSERT_NOT_NULL_MESSAGE(iso_core_table(&CORE, e->id), "a ready image without a table after the drain");
    }
    TEST_ASSERT_FALSE(up()->slot_active);
    TEST_ASSERT_EQUAL_UINT(0, iso_core_queue_len(&CORE));
    TEST_ASSERT_EQUAL_UINT(0, iso_core_index_pending(&CORE));
    TEST_ASSERT_EQUAL_INT(0, FS.open_files);
    check_images();
    check_view();
}

static void one_seed(uint32_t seed) {
    R.s = 0x9E3779B97F4A7C15ull ^ ((uint64_t)seed * 0xD1B54A32D192ED03ull);
    if (R.s == 0) R.s = 1;
    static const uint32_t chunks[] = {3000, 9000, 20000};
    static const uint32_t syncs[] = {0, 1, 5000, 15000};
    g_chunk = chunks[rnd(&R, 3)];
    g_sync = syncs[rnd(&R, 4)];
    g_bad_sig = rnd(&R, 6) == 0;
    fx_core_setup(g_chunk, g_sync);
    if (g_bad_sig) SIG = "CD002";
    memset(READERS, 0, sizeof(READERS));
    for (int step = 0; step < 150; step++) {
        st_ops++;
        uint32_t r = rnd(&R, 100);
        if (r < 22) {
            op_put(false);
        } else if (r < 26) {
            op_put(true);
        } else if (r < 32) {
            op_begin();
        } else if (r < 40) {
            op_complete();
        } else if (r < 50) {
            op_submit();
        } else if (r < 72) {
            op_worker();
        } else if (r < 80) {
            op_tick();
        } else if (r < 86) {
            op_reader();
        } else if (r < 88) {
            int at = 1 + (int)rnd(&R, 8);
            mock_tear_t tear = (mock_tear_t)rnd(&R, 3);
            mock_fs_cut_at(&FS, at, tear);
            st_cuts++;
        } else if (r < 89) {
            mock_fs_crash(&FS);
            st_crash++;
            TEST_ASSERT_EQUAL_INT(ISO_OK, iso_core_boot(&CORE));
        } else if (r < 90) {
            if (FS.cut_at == 0 && !FS.dead) {
                st_reinit++;
                TEST_ASSERT_EQUAL_INT(ISO_OK, iso_core_sd_reinit(&CORE, NULL, NULL));
            }
        } else if (r < 91) {
            if (FS.cut_at == 0 && !FS.dead) TEST_ASSERT_EQUAL_INT(ISO_OK, iso_core_boot(&CORE));
        } else {
            op_worker();
        }
        if (FS.dead) reboot_after_power_loss();
        check_all();
    }
    drain_and_check_quiescent();
    fx_core_teardown();
}

void test_fuzz_random_operations_across_the_worker_contract(void) {
    for (uint32_t seed = 1; seed <= 40; seed++) one_seed(seed);
    printf("core fuzz: ops=%d ready=%d invalid=%d deleted=%d cuts=%d crashes=%d reinit=%d finish_err=%d delete_busy=%d joined=%d overflow=%d readers=%d\n",
           st_ops, st_ready, st_invalid, st_deleted, st_cuts, st_crash, st_reinit, st_finish_err, st_delete_busy, st_joined, st_overflow, st_readers);
    TEST_ASSERT_EQUAL_INT(40 * 150, st_ops);
    TEST_ASSERT_TRUE(st_ready > 20);
    TEST_ASSERT_TRUE(st_invalid > 3);
    TEST_ASSERT_TRUE(st_deleted > 20);
    TEST_ASSERT_TRUE(st_cuts > 50);
    TEST_ASSERT_TRUE(st_crash > 20);
    TEST_ASSERT_TRUE(st_reinit > 20);
    TEST_ASSERT_TRUE(st_finish_err > 2);
    TEST_ASSERT_TRUE(st_joined > 5);
    TEST_ASSERT_TRUE(st_readers > 5);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_fuzz_random_operations_across_the_worker_contract);
    return UNITY_END();
}
