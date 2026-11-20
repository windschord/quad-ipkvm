/*
 * iso_upload の固定シードのファズ: ランダムな操作列(begin、PUT(正しい / 間違った offset)、アイドル、finish、削除、書き込みの失敗の注入、
 * エラーの注入、電源断の予約、即時の電源断、再起動)を、パーツの大きさ・チャンクの大きさ・sync の間隔を変えて実行する。
 * 操作のたびに、**ディスク上のバイト列を、実装を使わずに読んだ基準**(パーツのファイルの中身 = ソースの先頭、パーツの連鎖の構造、
 * 未同期の分と同期済みの分の大きさ)と、実装の状態(received、状態、枠、ハンドルの数、meta)を照合する。
 * ready になったイメージの SHA-256 は、ソースから別に計算した値と一致する。
 */
#include "up_common.h"

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

/* 集計(固定シードなので、毎回同じ) */
static int st_ops, st_cuts, st_ready, st_invalid, st_resumes, st_mismatch, st_write_fail, st_ioerr_finish, st_deleted, st_crash;

static void restart(void) {
    iso_upload_cfg_t cfg;
    fx_up_cfg(&cfg, g_chunk, g_sync);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_upload_init(&UP, &cfg));
    iso_upload_set_free(&UP, 64 * GIB, 60 * GIB);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_catalog_scan(&CAT));
}

/* deleting のイメージの削除を、最後まで行う。エラーで deleting が残るのは、電源断(FS.dead)のときだけ。戻り値は、削除できたか */
static bool delete_to_the_end(iso_catalog_entry_t *e) {
    iso_delete_job_t dj;
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_delete_job_init(&dj, e->id));
    iso_step_result_t r;
    int guard = 0;
    do {
        r = iso_catalog_delete_step(&CAT, &dj);
        TEST_ASSERT_TRUE(++guard < 1000);
    } while (r.kind == ISO_STEP_CONTINUE);
    if (r.kind == ISO_STEP_ERROR) TEST_ASSERT_TRUE_MESSAGE(FS.dead, "a delete failed without a power cut and left the image deleting");
    return r.kind == ISO_STEP_DONE;
}

/* 電源断の後(または即時の電源断の後)の再起動。deleting のイメージの削除は、続きを行う */
static void reboot(void) {
    mock_fs_power_on(&FS);
    mock_fs_reset_counter(&FS);
    mock_fs_fail_write(&FS, 0, 0, 0);
    restart();
    for (size_t i = 0; i < CAT.count;) {
        iso_catalog_entry_t *e = &CAT.images[i];
        if (e->state == ISO_STATE_DELETING) {
            if (!delete_to_the_end(e)) return; /* 電源断。呼び出し側が、もう一度再起動する */
            st_deleted++;
            continue; /* 配列が詰まる */
        }
        i++;
    }
}

/* ---- 基準による照合 ------------------------------------------------------------------------ */

typedef struct {
    uint64_t mem; /* メモリ上(未同期を含む)の連鎖の長さ */
    uint64_t dir; /* ディレクトリ上(同期済み)の連鎖の長さ */
} chain_t;

static chain_t check_chain(const iso_catalog_entry_t *e) {
    const uint64_t pbytes = (uint64_t)e->meta.part_blocks * 2048ull;
    chain_t c = {0, 0};
    bool prev_full = true, prev_full_dir = true, missing = false;
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
        if (ds > 0) TEST_ASSERT_TRUE_MESSAGE(prev_full_dir, "a synced part follows a part that is not fully synced");
        size_t dl = 0;
        const uint8_t *d = mock_fs_data(&FS, path, &dl);
        for (int64_t k = 0; k < ms; k++) {
            if (!d || (size_t)k >= dl || d[k] != gen_byte(c.mem + (uint64_t)k)) TEST_FAIL_MESSAGE("part content differs from the source");
        }
        c.mem += (uint64_t)ms;
        c.dir += (uint64_t)ds;
        prev_full = ((uint64_t)ms == pbytes);
        prev_full_dir = ((uint64_t)ds == pbytes);
    }
    return c;
}

static void check_all(void) {
    int active = 0;
    for (size_t i = 0; i < CAT.count; i++) {
        const iso_catalog_entry_t *e = &CAT.images[i];
        if (!e->meta_valid || e->state == ISO_STATE_DELETING) continue;
        iso_slots_t s;
        TEST_ASSERT_EQUAL_INT(ISO_OK, iso_slots_read(&IFS, e->id, &s));
        TEST_ASSERT_TRUE(s.adopted >= 0);
        switch (e->state) {
        case ISO_STATE_UPLOADING: {
            active++;
            chain_t c = check_chain(e);
            TEST_ASSERT_EQUAL_UINT64(c.mem, e->received); /* 進行中は、メモリ上の連鎖の長さと一致する */
            TEST_ASSERT_TRUE(UP.slot_active && !UP.slot_finalizing && strcmp(UP.slot_id, e->id) == 0);
            break;
        }
        case ISO_STATE_FINALIZING: active++; break;
        case ISO_STATE_INCOMPLETE: {
            chain_t c = check_chain(e);
            TEST_ASSERT_TRUE(e->received <= c.mem); /* 取り消せなかったときは、古い値(小さい)のことがある。大きい方に間違えない */
            TEST_ASSERT_TRUE(c.dir <= c.mem);
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
            TEST_ASSERT_EQUAL_UINT64(e->meta.size_bytes, c.mem);
            TEST_ASSERT_EQUAL_UINT64(e->meta.size_bytes, c.dir);
            break;
        }
        default: break;
        }
        if (s.meta[s.adopted].state == ISO_META_READY) TEST_ASSERT_EQUAL_INT(ISO_STATE_READY, e->state);
    }
    TEST_ASSERT_TRUE(active <= 1);
    /* 取り消せなかった truncate の保留: 全体で ISO_PEND_MAX 件以下、イメージごとに ISO_MAX_PARTS 件以下(捨てずに済む上限) */
    TEST_ASSERT_TRUE(UP.pend_n <= ISO_PEND_MAX);
    for (size_t i = 0; i < UP.pend_n; i++) {
        unsigned same = 0;
        for (size_t k = 0; k < UP.pend_n; k++) same += strcmp(UP.pend[i].id, UP.pend[k].id) == 0;
        TEST_ASSERT_TRUE(same <= ISO_MAX_PARTS);
    }
    TEST_ASSERT_EQUAL_INT(active, UP.slot_active ? 1 : 0);
    TEST_ASSERT_EQUAL_INT(UP.f ? 1 : 0, FS.open_files);
    TEST_ASSERT_TRUE(FS.open_dirs == 0);
    TEST_ASSERT_TRUE(CAT.count <= ISO_MAX_IMAGES);
}

/* ---- 操作 ------------------------------------------------------------------------------- */

static iso_catalog_entry_t *pick_image(iso_state_t a, iso_state_t b) {
    iso_catalog_entry_t *c[ISO_MAX_IMAGES];
    size_t n = 0;
    for (size_t i = 0; i < CAT.count; i++) {
        iso_catalog_entry_t *e = &CAT.images[i];
        if (e->meta_valid && (e->state == a || e->state == b)) c[n++] = e;
    }
    return n ? c[rnd(&R, (uint32_t)n)] : NULL;
}

static int armed_injection; /* この操作に注入したか */

static void inject_for_next_op(void) {
    armed_injection = 0;
    uint32_t r = rnd(&R, 100);
    if (r < 8) {
        int nth = 1 + (int)rnd(&R, 3);
        uint32_t limit = rnd(&R, 6000);
        int err = rnd(&R, 2) ? ISO_ERR_IO : ISO_ERR_NO_SPACE;
        mock_fs_fail_write(&FS, nth, limit, err);
        armed_injection = 1;
    } else if (r < 14) {
        mock_fs_error_at(&FS, 1 + (int)rnd(&R, 6), ISO_ERR_IO);
        armed_injection = 1;
    }
}

static void clear_injection(void) {
    mock_fs_fail_write(&FS, 0, 0, 0);
    mock_fs_reset_counter(&FS);
}

static void op_begin(void) {
    if (iso_upload_slot_busy(&UP) || CAT.count >= ISO_MAX_IMAGES - 2) return;
    static const uint32_t pbs[] = {PB_SMALL, 18, 25, 0};
    uint32_t pb = pbs[rnd(&R, 4)];
    uint64_t size = 34816 + rnd(&R, 120000);
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
    int rc = iso_upload_begin(&UP, name, strlen(name), size, decl, id);
    if (rc != ISO_OK) {
        TEST_ASSERT_TRUE(rc == ISO_RES_NAME_EXISTS || FS.dead || rc == ISO_ERR_IO || rc == ISO_ERR_EXISTS);
        return;
    }
    if (pb) {
        iso_catalog_entry_t *e = iso_catalog_find(&CAT, id);
        iso_meta_t m = e->meta;
        m.part_blocks = pb;
        m.part_count = (uint16_t)iso_extent_part_count(size, pb);
        (void)iso_catalog_write_meta(&CAT, e, &m); /* 電源断で失敗しうる(既定のパーツのまま) */
    }
}

static void op_put(bool wrong_offset) {
    iso_catalog_entry_t *e = NULL;
    if (UP.slot_active && !UP.slot_finalizing) {
        e = iso_catalog_find(&CAT, UP.slot_id);
    } else {
        e = pick_image(ISO_STATE_INCOMPLETE, ISO_STATE_INCOMPLETE);
    }
    if (!e || !e->meta_valid) return;
    iso_upload_info_t info;
    iso_upload_info(&UP, e->id, &info);
    if (!info.found || info.received >= info.size_bytes) return;
    uint64_t room = info.size_bytes - info.received;
    size_t len = 1 + rnd(&R, g_chunk);
    if (len > room) len = (size_t)room;
    uint64_t off = info.received;
    bool inject = !wrong_offset && rnd(&R, 100) < 20;
    if (inject) inject_for_next_op();
    if (wrong_offset) off = (off + 1 + rnd(&R, 3000)) % (info.size_bytes - 1);
    iso_upload_write_t w;
    bool was_incomplete = (e->state == ISO_STATE_INCOMPLETE);
    int rc = put_range(e->id, off, len, &w);
    if (rc == ISO_RES_OFFSET_MISMATCH) {
        st_mismatch++;
        TEST_ASSERT_TRUE_MESSAGE(wrong_offset || was_incomplete, "offset_mismatch for a correct offset on an uploading image");
        /* クライアントは、返された received から続ける */
        if (!armed_injection && !FS.dead) {
            uint64_t from = w.received;
            if (from < info.size_bytes) {
                size_t n = len;
                if (n > info.size_bytes - from) n = (size_t)(info.size_bytes - from);
                int rc2 = put_range(e->id, from, n, &w);
                TEST_ASSERT_TRUE_MESSAGE(rc2 == ISO_OK || rc2 == ISO_RES_UPLOAD_IN_PROGRESS || FS.dead, "retry from the reported received must succeed");
            }
        }
    } else if (rc == ISO_OK) {
        if (was_incomplete) st_resumes++;
        TEST_ASSERT_FALSE(wrong_offset && off != info.received);
    } else {
        /* 許される失敗: 注入、電源断、枠の使用中、書き込みの失敗(507 を含む) */
        if (rc == ISO_RES_INSUFFICIENT_STORAGE || rc == ISO_ERR_IO || rc == ISO_ERR_NO_SPACE) st_write_fail++;
        TEST_ASSERT_TRUE_MESSAGE(armed_injection || FS.dead || rc == ISO_RES_UPLOAD_IN_PROGRESS || rc == ISO_RES_OFFSET_MISMATCH, "unexpected failure of a PUT");
    }
    armed_injection = 0;
    clear_injection();
}

static void op_idle(void) {
    if (!UP.slot_active || UP.slot_finalizing) return;
    char id[9];
    memcpy(id, UP.slot_id, 9);
    inject_for_next_op();
    int rc = iso_upload_idle_close(&UP, id);
    TEST_ASSERT_TRUE(rc == ISO_OK || armed_injection || FS.dead);
    iso_catalog_entry_t *e = iso_catalog_find(&CAT, id);
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_INT(ISO_STATE_INCOMPLETE, e->state); /* 失敗しても incomplete */
    TEST_ASSERT_FALSE(UP.slot_active);
    armed_injection = 0;
    clear_injection();
}

static void op_finish(void) {
    iso_catalog_entry_t *e = NULL;
    if (UP.slot_active && !UP.slot_finalizing) e = iso_catalog_find(&CAT, UP.slot_id);
    if (!e || rnd(&R, 3) == 0) {
        iso_catalog_entry_t *o = pick_image(ISO_STATE_INCOMPLETE, ISO_STATE_INCOMPLETE);
        if (o) e = o;
    }
    if (!e || !e->meta_valid) return;
    char id[9];
    memcpy(id, e->id, 9);
    const uint64_t size = e->meta.size_bytes;
    uint8_t decl_sha[32];
    memcpy(decl_sha, e->meta.sha256, 32);
    bool decl = (e->meta.sha_flag == ISO_SHA_DECLARED);
    uint64_t rec = 0;
    int rc = iso_upload_finish_begin(&UP, id, &rec);
    if (rc == ISO_RES_SIZE_MISMATCH) {
        TEST_ASSERT_TRUE(rec < size);
        return;
    }
    if (rc != ISO_RES_OK) {
        TEST_ASSERT_TRUE(rc == ISO_RES_UPLOAD_IN_PROGRESS || rc < 0 || FS.dead);
        return;
    }
    TEST_ASSERT_EQUAL_UINT64(size, e->received);
    uint8_t want[32];
    gen_sha(size, want);
    bool predicted_checksum = decl && memcmp(decl_sha, want, 32) != 0;
    iso_finish_job_t job;
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_finish_job_init(&job, id));
    inject_for_next_op();
    uint32_t budget = rnd(&R, 3) == 0 ? 0 : 1 + rnd(&R, 20000);
    iso_step_result_t r;
    int guard = 0;
    do {
        r = iso_upload_finish_step(&UP, &job, budget, false);
        TEST_ASSERT_TRUE(++guard < 100000);
    } while (r.kind == ISO_STEP_CONTINUE);
    iso_finish_job_release(&UP, &job);
    armed_injection = armed_injection || FS.dead;
    if (r.kind == ISO_STEP_ERROR) {
        st_ioerr_finish++;
        TEST_ASSERT_TRUE(armed_injection);
        e = iso_catalog_find(&CAT, id);
        TEST_ASSERT_NOT_NULL(e);
        TEST_ASSERT_TRUE(e->state == ISO_STATE_INCOMPLETE || e->state == ISO_STATE_INVALID); /* invalid の meta 書き込みの失敗は、メモリ上 invalid */
        TEST_ASSERT_FALSE(UP.slot_active);
    } else {
        e = iso_catalog_find(&CAT, id);
        TEST_ASSERT_NOT_NULL(e);
        if (predicted_checksum) {
            TEST_ASSERT_EQUAL_INT(ISO_STATE_INVALID, e->state);
            TEST_ASSERT_EQUAL_UINT8(ISO_INVALID_CHECKSUM_MISMATCH, e->invalid_reason);
            st_invalid++;
        } else if (g_bad_sig) {
            TEST_ASSERT_EQUAL_INT(ISO_STATE_INVALID, e->state);
            TEST_ASSERT_EQUAL_UINT8(ISO_INVALID_NOT_ISO, e->invalid_reason);
            st_invalid++;
        } else {
            TEST_ASSERT_EQUAL_INT(ISO_STATE_READY, e->state);
            st_ready++;
        }
        TEST_ASSERT_FALSE(UP.slot_active);
    }
    armed_injection = 0;
    clear_injection();
    if (LAST_TABLE) {
        iso_extent_table_unref(LAST_TABLE);
        LAST_TABLE = NULL;
    }
}

/* DELETE の手順(iso_core が行う: 遷移 → 中止 → 削除のステップ) */
static void op_delete(void) {
    iso_catalog_entry_t *e = NULL;
    if (CAT.count) e = &CAT.images[rnd(&R, (uint32_t)CAT.count)];
    if (!e || e->state == ISO_STATE_DELETING) return;
    char id[9];
    memcpy(id, e->id, 9);
    iso_trans_ctx_t tc = {false, false, false};
    int rc = iso_catalog_apply(&CAT, e, ISO_EV_DELETE, &tc, NULL);
    if (rc != 0) return;
    iso_upload_abort(&UP, id);
    TEST_ASSERT_FALSE(UP.slot_active && strcmp(UP.slot_id, id) == 0); /* 他のイメージの枠は、そのまま */
    if (rnd(&R, 4) == 0) return; /* 削除のステップに進む前に再起動、などで残る(後で続ける) */
    inject_for_next_op();
    iso_delete_job_t dj;
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_delete_job_init(&dj, id));
    iso_step_result_t r;
    int guard = 0;
    do {
        r = iso_catalog_delete_step(&CAT, &dj);
        TEST_ASSERT_TRUE(++guard < 1000);
    } while (r.kind == ISO_STEP_CONTINUE);
    if (r.kind == ISO_STEP_DONE) st_deleted++;
    armed_injection = 0;
    clear_injection();
}

static void finish_pending_deletes(void) {
    for (size_t i = 0; i < CAT.count;) {
        iso_catalog_entry_t *e = &CAT.images[i];
        if (e->state == ISO_STATE_DELETING && !FS.dead) {
            if (!delete_to_the_end(e)) return; /* 電源断 */
            st_deleted++;
            continue;
        }
        i++;
    }
}

static void one_seed(uint32_t seed) {
    R.s = 0x9E3779B97F4A7C15ull ^ ((uint64_t)seed * 0xD1B54A32D192ED03ull);
    if (R.s == 0) R.s = 1;
    static const uint32_t chunks[] = {3000, 9000, 20000, 131072};
    static const uint32_t syncs[] = {0, 1, 5000, 15000};
    g_chunk = chunks[rnd(&R, 4)];
    g_sync = syncs[rnd(&R, 4)];
    g_bad_sig = rnd(&R, 6) == 0;
    fx_up_setup(g_chunk, g_sync);
    if (g_bad_sig) SIG = "CD002";
    for (int step = 0; step < 120; step++) {
        st_ops++;
        uint32_t r = rnd(&R, 100);
        if (r < 40) {
            op_put(false);
        } else if (r < 47) {
            op_put(true);
        } else if (r < 54) {
            op_begin();
        } else if (r < 60) {
            op_idle();
        } else if (r < 72) {
            op_finish();
        } else if (r < 78) {
            op_delete();
        } else if (r < 84) {
            /* 電源断の予約 */
            int at = 1 + (int)rnd(&R, 8);
            mock_tear_t tear = (mock_tear_t)rnd(&R, 3);
            mock_fs_cut_at(&FS, at, tear);
            st_cuts++;
        } else if (r < 88) {
            mock_fs_crash(&FS); /* いま電源が落ちた */
            st_crash++;
            restart();
            finish_pending_deletes();
        } else if (r < 92) {
            finish_pending_deletes();
        } else if (FS.cut_at == 0 && !FS.dead) { /* スキャンも mkdir(EXISTS)を書き込みとして数えるので、電源断の予約中は行わない */
            /* ディスクから見える状態(再起動の後の状態)の検査 */
            iso_catalog_t c2;
            iso_catalog_init(&c2, &IFS, &IRND);
            TEST_ASSERT_EQUAL_INT(ISO_OK, iso_catalog_scan(&c2));
            for (size_t i = 0; i < c2.count; i++) {
                const iso_catalog_entry_t *s = &c2.images[i];
                TEST_ASSERT_TRUE(s->state != ISO_STATE_UPLOADING && s->state != ISO_STATE_FINALIZING);
                if (s->meta_valid && s->state == ISO_STATE_INCOMPLETE) {
                    const iso_catalog_entry_t *l = iso_catalog_find(&CAT, s->id);
                    if (l && l->state == ISO_STATE_INCOMPLETE) TEST_ASSERT_TRUE(s->received <= check_chain(l).mem);
                }
            }
        }
        if (FS.dead) {
            reboot(); /* 電源断が起きた */
        }
        check_all();
    }
    /* 後始末: 進行中のアップロードを止め、削除を終わらせる */
    mock_fs_power_on(&FS);
    clear_injection();
    finish_pending_deletes();
    if (LAST_TABLE) {
        iso_extent_table_unref(LAST_TABLE);
        LAST_TABLE = NULL;
    }
    fx_up_teardown();
}

void test_fuzz_random_operations_with_power_cuts_and_failures(void) {
    for (uint32_t seed = 1; seed <= 100; seed++) one_seed(seed);
    /* 掃引が、主な場合に当たっている(固定シードなので、毎回同じ) */
    TEST_ASSERT_EQUAL_INT(100 * 120, st_ops);
    TEST_ASSERT_TRUE(st_ready > 50);
    TEST_ASSERT_TRUE(st_invalid > 5);
    TEST_ASSERT_TRUE(st_resumes > 50);
    TEST_ASSERT_TRUE(st_mismatch > 20);
    TEST_ASSERT_TRUE(st_write_fail > 20);
    TEST_ASSERT_TRUE(st_ioerr_finish > 3);
    TEST_ASSERT_TRUE(st_cuts > 200);
    TEST_ASSERT_TRUE(st_crash > 50);
    TEST_ASSERT_TRUE(st_deleted > 20);
    printf("fuzz: ops=%d cuts=%d crashes=%d ready=%d invalid=%d resumes=%d mismatch=%d write_fail=%d finish_ioerr=%d deleted=%d\n", st_ops,
           st_cuts, st_crash, st_ready, st_invalid, st_resumes, st_mismatch, st_write_fail, st_ioerr_finish, st_deleted);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_fuzz_random_operations_with_power_cuts_and_failures);
    return UNITY_END();
}
