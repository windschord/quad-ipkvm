/*
 * finish(WEB_API 8.4、設計 3.4): iso_upload_finish_begin(受付と遷移)と、FINISH ジョブのステップ(iso_upload_finish_step。
 * sync + close → SHA-256(逐次の状態、または全パーツの読み直し)→ 宣言値との比較 → 署名 → エクステント → meta を ready で書く)。
 * 期待値は手書き。SHA-256 の定数は Python の hashlib で、同じ生成規則(up_common.h の gen_byte)から別に求めた。
 */
#include "up_common.h"

void setUp(void) { fx_up_setup(0, 0); }
void tearDown(void) { fx_up_teardown(); }

#define SZ 100000ull
#define SHA_100000 "e141cf78e32486f3c90e3dcf7c6f47afbd0c52777785a4d649ccc3333e4fdaac"
#define SHA_100000_BEA01 "284ffdd8dbd07cf7e4f93c468259dbbb36b6081b5da7dad4fa69232481f2cbba"
#define SHA_34816 "fb3d44988cca46e3fc48cbdc687fe16fe388f8f98949dcfae3edd8145d8fb9a2"

static char ID[ISO_ID_LEN + 1];

static iso_catalog_entry_t *entry(void) { return iso_catalog_find(&CAT, ID); }

static void upload_all(uint64_t size, uint32_t pb) {
    img_begin_pb("img", size, pb, NULL, ID);
    TEST_ASSERT_EQUAL_STRING("11110001", ID);
    put_all(ID, 0, size, 20000);
}

static iso_step_result_t run_finish(iso_finish_job_t *job, uint32_t budget, int *steps_out) {
    iso_step_result_t r;
    int steps = 0;
    do {
        r = iso_upload_finish_step(&UP, job, budget, false);
        steps++;
        TEST_ASSERT_TRUE_MESSAGE(steps < 100000, "finish_step did not end");
    } while (r.kind == ISO_STEP_CONTINUE);
    if (steps_out) *steps_out = steps;
    return r;
}

/* finish の受付 + ステップを最後まで */
static iso_step_result_t finish_all(uint32_t budget) {
    TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_upload_finish_begin(&UP, ID, NULL));
    iso_finish_job_t job;
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_finish_job_init(&job, ID));
    return run_finish(&job, budget, NULL);
}

/* 再起動の後と同じ(メモリの状態を失い、カタログを作り直す) */
static void reboot(void) {
    iso_upload_cfg_t cfg;
    fx_up_cfg(&cfg, 0, 0);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_upload_init(&UP, &cfg));
    iso_upload_set_free(&UP, 64 * GIB, 60 * GIB);
    mock_fs_drop_handles(&FS);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_catalog_scan(&CAT));
}

static void expect_meta(uint8_t state, uint8_t reason) {
    iso_slots_t s;
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_slots_read(&IFS, ID, &s));
    TEST_ASSERT_TRUE(s.adopted >= 0);
    TEST_ASSERT_EQUAL_UINT8(state, s.meta[s.adopted].state);
    TEST_ASSERT_EQUAL_UINT8(reason, s.meta[s.adopted].invalid_reason);
}

static void expect_ready(const char *sha_hex, uint16_t parts) {
    iso_catalog_entry_t *e = entry();
    TEST_ASSERT_EQUAL_INT(ISO_STATE_READY, e->state);
    TEST_ASSERT_EQUAL_UINT64(e->meta.size_bytes, e->received);
    iso_slots_t s;
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_slots_read(&IFS, ID, &s));
    const iso_meta_t *m = &s.meta[s.adopted];
    TEST_ASSERT_EQUAL_UINT8(ISO_META_READY, m->state);
    TEST_ASSERT_EQUAL_UINT8(ISO_SHA_COMPUTED, m->sha_flag);
    uint8_t want[32];
    hex32(sha_hex, want);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, m->sha256, 32);
    TEST_ASSERT_EQUAL_UINT16(parts, m->part_count);
    TEST_ASSERT_EQUAL_INT(1, TABLES_READY);
    TEST_ASSERT_EQUAL_STRING(ID, TABLE_ID);
    TEST_ASSERT_NOT_NULL(LAST_TABLE);
    TEST_ASSERT_EQUAL_UINT32(1, iso_extent_table_refs(LAST_TABLE));
    TEST_ASSERT_EQUAL_UINT32(parts, iso_extent_table_part_count(LAST_TABLE));
    TEST_ASSERT_EQUAL_UINT64(m->size_bytes, iso_extent_table_size_bytes(LAST_TABLE));
    TEST_ASSERT_EQUAL_UINT32(EPOCH, iso_extent_table_epoch(LAST_TABLE));
    TEST_ASSERT_FALSE(iso_upload_slot_busy(&UP));
    TEST_ASSERT_EQUAL_INT(0, FS.open_files);
}

static void expect_invalid(uint8_t reason) {
    iso_catalog_entry_t *e = entry();
    TEST_ASSERT_EQUAL_INT(ISO_STATE_INVALID, e->state);
    TEST_ASSERT_EQUAL_UINT8(reason, e->invalid_reason);
    expect_meta(ISO_META_INVALID, reason);
    TEST_ASSERT_EQUAL_INT(0, TABLES_READY);
    TEST_ASSERT_FALSE(iso_upload_slot_busy(&UP));
    TEST_ASSERT_EQUAL_INT(0, FS.open_files);
    TEST_ASSERT_EQUAL_INT(0, ALLOC_LIVE);
}

static void expect_incomplete_after_ioerr(void) {
    iso_catalog_entry_t *e = entry();
    TEST_ASSERT_EQUAL_INT(ISO_STATE_INCOMPLETE, e->state);
    TEST_ASSERT_EQUAL_INT(0, TABLES_READY);
    TEST_ASSERT_FALSE(iso_upload_slot_busy(&UP));
    TEST_ASSERT_EQUAL_INT(0, FS.open_files);
    TEST_ASSERT_EQUAL_INT(0, ALLOC_LIVE);
}

/* ---- 受付(iso_upload_finish_begin) ------------------------------------------------------- */

void test_finish_begin_complete_upload_becomes_finalizing_and_keeps_the_slot(void) {
    upload_all(SZ, PB_SMALL);
    uint64_t rec = 0;
    TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_upload_finish_begin(&UP, ID, &rec));
    TEST_ASSERT_EQUAL_UINT64(SZ, rec);
    TEST_ASSERT_EQUAL_INT(ISO_STATE_FINALIZING, entry()->state);
    TEST_ASSERT_TRUE(iso_upload_slot_busy(&UP)); /* finalizing も枠を使う */
    TEST_ASSERT_TRUE(UP.slot_finalizing);
    TEST_ASSERT_EQUAL_INT(ISO_UP_ACT_FINALIZING, iso_upload_activity(&UP, NOW_MS));
    expect_meta(ISO_META_UPLOADING, 0); /* meta は ready になるまで変えない */
    /* 冪等 */
    TEST_ASSERT_EQUAL_INT(ISO_RES_ALREADY_FINALIZING, iso_upload_finish_begin(&UP, ID, NULL));
    /* finalizing の間は、PUT も begin も受け付けない */
    iso_upload_write_t w;
    TEST_ASSERT_EQUAL_INT(ISO_RES_NOT_UPLOADING, put_range(ID, SZ, 1, &w));
    char id2[9];
    TEST_ASSERT_EQUAL_INT(ISO_RES_UPLOAD_IN_PROGRESS, iso_upload_begin(&UP, "two", 3, SZ, NULL, id2));
}

void test_finish_begin_size_mismatch_reports_received(void) {
    img_begin_pb("img", SZ, PB_SMALL, NULL, ID);
    put_all(ID, 0, SZ - 1, 20000);
    uint64_t rec = 0;
    TEST_ASSERT_EQUAL_INT(ISO_RES_SIZE_MISMATCH, iso_upload_finish_begin(&UP, ID, &rec));
    TEST_ASSERT_EQUAL_UINT64(SZ - 1, rec);
    TEST_ASSERT_EQUAL_INT(ISO_STATE_UPLOADING, entry()->state); /* 変化なし */
    TEST_ASSERT_TRUE(iso_upload_slot_busy(&UP));
    TEST_ASSERT_FALSE(UP.slot_finalizing);
    iso_upload_write_t w;
    TEST_ASSERT_EQUAL_INT(ISO_OK, put_range(ID, SZ - 1, 1, &w));
    TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_upload_finish_begin(&UP, ID, &rec));
}

void test_finish_begin_incomplete_with_less_than_size_is_size_mismatch(void) {
    img_begin_pb("img", SZ, PB_SMALL, NULL, ID);
    put_all(ID, 0, 5000, 5000);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_upload_idle_close(&UP, ID));
    uint64_t rec = 0;
    TEST_ASSERT_EQUAL_INT(ISO_RES_SIZE_MISMATCH, iso_upload_finish_begin(&UP, ID, &rec));
    TEST_ASSERT_EQUAL_UINT64(5000, rec);
    TEST_ASSERT_EQUAL_INT(ISO_STATE_INCOMPLETE, entry()->state);
    TEST_ASSERT_FALSE(iso_upload_slot_busy(&UP));
}

void test_finish_begin_incomplete_complete_takes_the_free_slot(void) {
    upload_all(SZ, PB_SMALL);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_upload_idle_close(&UP, ID));
    TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_upload_finish_begin(&UP, ID, NULL));
    TEST_ASSERT_EQUAL_INT(ISO_STATE_FINALIZING, entry()->state);
    TEST_ASSERT_TRUE(UP.slot_finalizing);
    TEST_ASSERT_EQUAL_STRING(ID, iso_upload_slot_id(&UP));
}

void test_finish_begin_incomplete_complete_with_the_slot_used_is_upload_in_progress(void) {
    upload_all(SZ, PB_SMALL);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_upload_idle_close(&UP, ID));
    char id2[9];
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_upload_begin(&UP, "other", 5, SZ, NULL, id2));
    TEST_ASSERT_EQUAL_INT(ISO_RES_UPLOAD_IN_PROGRESS, iso_upload_finish_begin(&UP, ID, NULL));
    TEST_ASSERT_EQUAL_INT(ISO_STATE_INCOMPLETE, entry()->state);
    TEST_ASSERT_EQUAL_STRING(id2, iso_upload_slot_id(&UP));
    TEST_ASSERT_FALSE(UP.slot_finalizing);
}

void test_finish_begin_rereads_received_for_an_incomplete_image(void) {
    upload_all(SZ, PB_SMALL);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_upload_idle_close(&UP, ID));
    entry()->received = 5; /* メモリの値が狂っていても */
    TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_upload_finish_begin(&UP, ID, NULL));
    TEST_ASSERT_EQUAL_UINT64(SZ, entry()->received);
}

void test_finish_begin_other_states(void) {
    upload_all(SZ, PB_SMALL);
    iso_catalog_entry_t *e = entry();
    TEST_ASSERT_EQUAL_INT(ISO_RES_NOT_FOUND, iso_upload_finish_begin(&UP, "ffffffff", NULL));
    e->state = ISO_STATE_READY;
    TEST_ASSERT_EQUAL_INT(ISO_RES_ALREADY_READY, iso_upload_finish_begin(&UP, ID, NULL));
    e->state = ISO_STATE_INVALID;
    TEST_ASSERT_EQUAL_INT(ISO_RES_INVALID_IMAGE, iso_upload_finish_begin(&UP, ID, NULL));
    e->state = ISO_STATE_DELETING;
    TEST_ASSERT_EQUAL_INT(ISO_RES_NOT_FOUND, iso_upload_finish_begin(&UP, ID, NULL));
    e->state = ISO_STATE_FINALIZING;
    TEST_ASSERT_EQUAL_INT(ISO_RES_ALREADY_FINALIZING, iso_upload_finish_begin(&UP, ID, NULL));
    e->state = ISO_STATE_UPLOADING;
    TEST_ASSERT_FALSE(UP.slot_finalizing); /* どの拒否も、枠を変えない */
}

void test_finish_begin_reload_error_changes_nothing(void) {
    upload_all(SZ, PB_SMALL);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_upload_idle_close(&UP, ID));
    mock_fs_fail_path(&FS, "/iso/11110001/p000");
    TEST_ASSERT_EQUAL_INT(ISO_ERR_IO, iso_upload_finish_begin(&UP, ID, NULL));
    TEST_ASSERT_EQUAL_INT(ISO_STATE_INCOMPLETE, entry()->state);
    TEST_ASSERT_FALSE(iso_upload_slot_busy(&UP));
    mock_fs_fail_path(&FS, NULL);
}

void test_finish_undo_restores_the_state_and_the_slot(void) {
    upload_all(SZ, PB_SMALL);
    iso_catalog_entry_t *e = entry();
    TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_upload_finish_begin(&UP, ID, NULL));
    iso_upload_finish_undo(&UP, e, ISO_STATE_UPLOADING);
    TEST_ASSERT_EQUAL_INT(ISO_STATE_UPLOADING, e->state);
    TEST_ASSERT_TRUE(iso_upload_slot_busy(&UP));
    TEST_ASSERT_FALSE(UP.slot_finalizing);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_upload_idle_close(&UP, ID));
    TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_upload_finish_begin(&UP, ID, NULL));
    iso_upload_finish_undo(&UP, e, ISO_STATE_INCOMPLETE);
    TEST_ASSERT_EQUAL_INT(ISO_STATE_INCOMPLETE, e->state);
    TEST_ASSERT_FALSE(iso_upload_slot_busy(&UP));
}

/* ---- ステップ: 成功 -------------------------------------------------------------------------- */

void test_finish_with_the_streamed_sha_reaches_ready(void) {
    upload_all(SZ, PB_SMALL);
    FS.n_read_at = 0;
    iso_step_result_t r = finish_all(0);
    TEST_ASSERT_EQUAL_INT(ISO_STEP_DONE, r.kind);
    expect_ready(SHA_100000, 3);
    TEST_ASSERT_EQUAL_INT(1, FS.n_read_at); /* 署名の読み出しだけ。SHA-256 は逐次の状態から */
    TEST_ASSERT_EQUAL_UINT64(32768, FS.last_read_at_offset);
    TEST_ASSERT_FALSE(UP.sha_valid);
    /* 書き込み用のハンドルを閉じて、全パーツが同期済み */
    expect_disk_prefix(ID, PB_SMALL, SZ);
    TEST_ASSERT_EQUAL_INT64(30368, mock_fs_size(&FS, "/iso/11110001/p002"));
}

void test_finish_refreshes_the_free_space_cache(void) {
    upload_all(SZ, PB_SMALL);
    TEST_ASSERT_TRUE(UP.consumed_since > 0);
    mock_fs_set_mount_info(&FS, 64 * GIB, 50 * GIB, 32768);
    iso_step_result_t r = finish_all(0);
    TEST_ASSERT_EQUAL_INT(ISO_STEP_DONE, r.kind);
    TEST_ASSERT_EQUAL_UINT64(50 * GIB, UP.free_bytes);
    TEST_ASSERT_EQUAL_UINT64(0, UP.consumed_since);
}

void test_finish_with_a_matching_declared_sha_reaches_ready_and_stores_the_computed_flag(void) {
    uint8_t sha[32];
    hex32(SHA_100000, sha);
    img_begin_pb("img", SZ, PB_SMALL, sha, ID);
    put_all(ID, 0, SZ, 20000);
    iso_step_result_t r = finish_all(0);
    TEST_ASSERT_EQUAL_INT(ISO_STEP_DONE, r.kind);
    expect_ready(SHA_100000, 3);
}

void test_finish_after_a_reboot_recomputes_the_sha_in_budget_steps(void) {
    upload_all(SZ, PB_SMALL);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_upload_idle_close(&UP, ID));
    reboot(); /* SHA-256 の状態は失われる。received == size の incomplete */
    TEST_ASSERT_EQUAL_INT(ISO_STATE_INCOMPLETE, entry()->state);
    TEST_ASSERT_EQUAL_UINT64(SZ, entry()->received);
    FS.n_read_at = 0;
    TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_upload_finish_begin(&UP, ID, NULL));
    iso_finish_job_t job;
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_finish_job_init(&job, ID));
    int steps = 0;
    iso_step_result_t r = run_finish(&job, 4096, &steps);
    TEST_ASSERT_EQUAL_INT(ISO_STEP_DONE, r.kind);
    expect_ready(SHA_100000, 3);
    /* 4096 バイトずつ: p000 (34816) = 9 回、p001 = 9 回、p002 (30368) = 8 回、署名 1 回 */
    TEST_ASSERT_EQUAL_INT(27, FS.n_read_at);
    TEST_ASSERT_TRUE(steps > 26);
}

void test_finish_after_a_reboot_with_a_larger_budget_makes_fewer_steps(void) {
    upload_all(SZ, PB_SMALL);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_upload_idle_close(&UP, ID));
    reboot();
    FS.n_read_at = 0;
    TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_upload_finish_begin(&UP, ID, NULL));
    iso_finish_job_t job;
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_finish_job_init(&job, ID));
    iso_step_result_t r = run_finish(&job, 10000, NULL);
    TEST_ASSERT_EQUAL_INT(ISO_STEP_DONE, r.kind);
    expect_ready(SHA_100000, 3);
    /* 1 ステップ 10000 バイト(4096 + 4096 + 1808): p000 11 回、p001 11 回、p002 10 回、署名 1 回 */
    TEST_ASSERT_EQUAL_INT(33, FS.n_read_at);
}

void test_hash_step_budget_zero_still_progresses(void) {
    upload_all(SZ, PB_SMALL);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_upload_idle_close(&UP, ID));
    reboot();
    iso_finish_job_t job;
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_finish_job_init(&job, ID));
    iso_step_result_t r = iso_upload_hash_step(&UP, &job, 0);
    TEST_ASSERT_EQUAL_INT(ISO_STEP_CONTINUE, r.kind);
    TEST_ASSERT_EQUAL_UINT64(4096, job.hash_off); /* budget 0 = 読み出しバッファ 1 回分 */
    TEST_ASSERT_EQUAL_INT(0, FS.open_files);      /* ステップの間は、読み取りのハンドルを持たない */
}

void test_hash_step_runs_to_the_digest_across_parts(void) {
    upload_all(SZ, PB_SMALL);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_upload_idle_close(&UP, ID));
    reboot();
    iso_finish_job_t job;
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_finish_job_init(&job, ID));
    iso_step_result_t r;
    int steps = 0;
    do {
        r = iso_upload_hash_step(&UP, &job, 40000);
        steps++;
        TEST_ASSERT_EQUAL_INT(0, FS.open_files);
    } while (r.kind == ISO_STEP_CONTINUE && steps < 100);
    TEST_ASSERT_EQUAL_INT(ISO_STEP_DONE, r.kind);
    TEST_ASSERT_EQUAL_INT(ISO_OUTCOME_NONE, job.fail);
    TEST_ASSERT_TRUE(job.have_digest);
    uint8_t want[32];
    hex32(SHA_100000, want);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, job.digest, 32);
    /* 1 ステップは 1 パーツまで(budget が残っていても、次のパーツは次のステップ): 3 パーツ = 3 ステップ(34816 バイトは 40000 に収まる) */
    TEST_ASSERT_EQUAL_INT(3, steps);
}

void test_resumed_upload_after_idle_still_streams_the_sha(void) {
    img_begin_pb("img", SZ, PB_SMALL, NULL, ID);
    put_all(ID, 0, 50000, 20000);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_upload_idle_close(&UP, ID));
    put_all(ID, 50000, SZ, 20000);
    FS.n_read_at = 0;
    iso_step_result_t r = finish_all(0);
    TEST_ASSERT_EQUAL_INT(ISO_STEP_DONE, r.kind);
    expect_ready(SHA_100000, 3);
    TEST_ASSERT_EQUAL_INT(1, FS.n_read_at); /* アイドルでは SHA-256 の状態を保つ */
}

void test_finish_after_a_failed_and_rolled_back_chunk_still_streams(void) {
    img_begin_pb("img", SZ, PB_SMALL, NULL, ID);
    put_all(ID, 0, 20000, 20000);
    mock_fs_fail_write(&FS, 1, 100, ISO_ERR_NO_SPACE);
    iso_upload_write_t w;
    TEST_ASSERT_EQUAL_INT(ISO_RES_INSUFFICIENT_STORAGE, put_range(ID, 20000, 20000, &w));
    mock_fs_fail_write(&FS, 0, 0, 0);
    TEST_ASSERT_EQUAL_UINT64(20000, w.received);
    put_all(ID, 20000, SZ, 20000);
    FS.n_read_at = 0;
    iso_step_result_t r = finish_all(0);
    TEST_ASSERT_EQUAL_INT(ISO_STEP_DONE, r.kind);
    expect_ready(SHA_100000, 3);
    TEST_ASSERT_EQUAL_INT(1, FS.n_read_at); /* truncate で取り消したので、received と SHA-256 の状態は合っている */
}

void test_finish_after_an_unrolled_partial_write_recomputes_the_sha(void) {
    img_begin_pb("img", SZ, PB_SMALL, NULL, ID);
    put_all(ID, 0, 20000, 20000);
    fx_settle();
    mock_fs_fail_write(&FS, 1, 100, ISO_ERR_NO_SPACE);
    mock_fs_error_at(&FS, 3, ISO_ERR_IO); /* write(1)、close(2)、truncate(3) が失敗 */
    iso_upload_write_t w;
    TEST_ASSERT_EQUAL_INT(ISO_RES_INSUFFICIENT_STORAGE, put_range(ID, 20000, 20000, &w));
    mock_fs_fail_write(&FS, 0, 0, 0);
    mock_fs_reset_counter(&FS);
    TEST_ASSERT_EQUAL_UINT64(20100, w.received);
    put_all(ID, 20100, SZ, 20000);
    FS.n_read_at = 0;
    iso_step_result_t r = finish_all(0);
    TEST_ASSERT_EQUAL_INT(ISO_STEP_DONE, r.kind);
    expect_ready(SHA_100000, 3); /* 取り消せなかった 100 バイトを含むデータの、正しい SHA-256 */
    TEST_ASSERT_TRUE(FS.n_read_at > 20); /* 読み直した */
}

void test_minimal_image_34816_bytes_one_part(void) {
    upload_all(34816, 0); /* 既定のパーツ(1 GiB)のまま */
    iso_step_result_t r = finish_all(0);
    TEST_ASSERT_EQUAL_INT(ISO_STEP_DONE, r.kind);
    expect_ready(SHA_34816, 1);
}

void test_finish_step_sequence_closes_before_reading(void) {
    /* FatFs の排他: 書き込み用のハンドルが開いたままだと、モックは open_for_read / get_extents を BUSY にする */
    upload_all(SZ, PB_SMALL);
    iso_step_result_t r = finish_all(0);
    TEST_ASSERT_EQUAL_INT(ISO_STEP_DONE, r.kind);
    TEST_ASSERT_EQUAL_INT(ISO_STATE_READY, entry()->state);
}

void test_signature_bea01_is_accepted(void) {
    SIG = "BEA01";
    upload_all(SZ, PB_SMALL);
    iso_step_result_t r = finish_all(0);
    TEST_ASSERT_EQUAL_INT(ISO_STEP_DONE, r.kind);
    expect_ready(SHA_100000_BEA01, 3);
}

void test_signature_in_a_later_part_when_parts_are_small(void) {
    /* パーツ 8 ブロック(16384 バイト): セクタ 16 (32768) は p002 の先頭 */
    upload_all(SZ, 8);
    TEST_ASSERT_EQUAL_UINT16(7, entry()->meta.part_count);
    FS.n_read_at = 0;
    iso_step_result_t r = finish_all(0);
    TEST_ASSERT_EQUAL_INT(ISO_STEP_DONE, r.kind);
    expect_ready(SHA_100000, 7);
    TEST_ASSERT_EQUAL_UINT64(0, FS.last_read_at_offset); /* p002 の中の位置 */
}

/* ---- ステップ: 検査の失敗(invalid) ------------------------------------------------------------ */

void test_wrong_signature_is_not_iso(void) {
    static const char *bad[] = {"CD002", "cd001", "XD001", "CD00X", "BEA02", "     "};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        fx_up_teardown();
        fx_up_setup(0, 0);
        SIG = bad[i];
        upload_all(SZ, PB_SMALL);
        ALLOC_CALLS = 0;
        iso_step_result_t r = finish_all(0);
        TEST_ASSERT_EQUAL_INT(ISO_STEP_DONE, r.kind);
        expect_invalid(ISO_INVALID_NOT_ISO);
        TEST_ASSERT_EQUAL_INT(0, ALLOC_CALLS); /* エクステントの作成まで進まない */
    }
}

void test_declared_sha_mismatch_with_streamed_state(void) {
    uint8_t sha[32];
    hex32(SHA_100000, sha);
    sha[31] ^= 1; /* 最後の 1 ビットだけ違う */
    img_begin_pb("img", SZ, PB_SMALL, sha, ID);
    put_all(ID, 0, SZ, 20000);
    ALLOC_CALLS = 0;
    FS.n_read_at = 0;
    iso_step_result_t r = finish_all(0);
    TEST_ASSERT_EQUAL_INT(ISO_STEP_DONE, r.kind);
    expect_invalid(ISO_INVALID_CHECKSUM_MISMATCH);
    TEST_ASSERT_EQUAL_INT(0, ALLOC_CALLS);
    TEST_ASSERT_EQUAL_INT(0, FS.n_read_at); /* 署名の読み出しにも進まない */
}

void test_declared_sha_mismatch_in_the_first_byte(void) {
    uint8_t sha[32];
    hex32(SHA_100000, sha);
    sha[0] ^= 0x80;
    img_begin_pb("img", SZ, PB_SMALL, sha, ID);
    put_all(ID, 0, SZ, 20000);
    iso_step_result_t r = finish_all(0);
    TEST_ASSERT_EQUAL_INT(ISO_STEP_DONE, r.kind);
    expect_invalid(ISO_INVALID_CHECKSUM_MISMATCH);
}

void test_declared_sha_mismatch_with_recomputed_digest(void) {
    uint8_t sha[32];
    hex32(SHA_100000, sha);
    sha[15] ^= 0x10;
    img_begin_pb("img", SZ, PB_SMALL, sha, ID);
    put_all(ID, 0, SZ, 20000);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_upload_idle_close(&UP, ID));
    reboot();
    iso_step_result_t r = finish_all(0);
    TEST_ASSERT_EQUAL_INT(ISO_STEP_DONE, r.kind);
    expect_invalid(ISO_INVALID_CHECKSUM_MISMATCH);
}

void test_declared_sha_matches_after_a_reboot(void) {
    uint8_t sha[32];
    hex32(SHA_100000, sha);
    img_begin_pb("img", SZ, PB_SMALL, sha, ID);
    put_all(ID, 0, SZ, 20000);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_upload_idle_close(&UP, ID));
    reboot(); /* 宣言値(フラグ 1)は meta に保存されているので、再起動しても残る */
    TEST_ASSERT_EQUAL_UINT8(ISO_SHA_DECLARED, entry()->meta.sha_flag);
    iso_step_result_t r = finish_all(0);
    TEST_ASSERT_EQUAL_INT(ISO_STEP_DONE, r.kind);
    expect_ready(SHA_100000, 3);
}

void test_undeclared_sha_has_no_comparison(void) {
    upload_all(SZ, PB_SMALL);
    TEST_ASSERT_EQUAL_UINT8(ISO_SHA_NONE, entry()->meta.sha_flag);
    iso_step_result_t r = finish_all(0);
    TEST_ASSERT_EQUAL_INT(ISO_STEP_DONE, r.kind);
    TEST_ASSERT_EQUAL_INT(ISO_STATE_READY, entry()->state);
}

void test_invalid_image_stays_invalid_for_later_finish(void) {
    SIG = "XXXXX";
    upload_all(SZ, PB_SMALL);
    iso_step_result_t r = finish_all(0);
    TEST_ASSERT_EQUAL_INT(ISO_STEP_DONE, r.kind);
    TEST_ASSERT_EQUAL_INT(ISO_RES_INVALID_IMAGE, iso_upload_finish_begin(&UP, ID, NULL));
    reboot();
    TEST_ASSERT_EQUAL_INT(ISO_STATE_INVALID, entry()->state);
    TEST_ASSERT_EQUAL_UINT8(ISO_INVALID_NOT_ISO, entry()->invalid_reason);
}

/* ---- エクステントの失敗 -------------------------------------------------------------------- */

static int g_frag_n;
static int frag_get_extents(void *ctx, const char *path, iso_extent_t *out, size_t max, size_t *n) {
    (void)ctx;
    if (!strstr(path, "p000")) {
        *n = 1;
        if (max < 1) return ISO_ERR_TOO_FRAGMENTED;
        out[0].start_sector = 100000;
        out[0].sector_count = 100000; /* 十分に大きい 1 本 */
        return ISO_OK;
    }
    *n = (size_t)g_frag_n;
    if ((size_t)g_frag_n > max) return ISO_ERR_TOO_FRAGMENTED;
    for (int i = 0; i < g_frag_n; i++) {
        out[i].start_sector = 1000 + 2u * (uint64_t)i;
        out[i].sector_count = 1;
    }
    return ISO_OK;
}

void test_extent_count_at_the_limit_is_accepted(void) {
    /* パーツ 1024 ブロック = 2 MiB = 4096 セクタ。1 セクタずつの 4096 本ちょうどは受理 */
    IFS.get_extents = frag_get_extents;
    g_frag_n = 4096;
    upload_all(1024ull * 2048, 1024);
    iso_step_result_t r = finish_all(0);
    TEST_ASSERT_EQUAL_INT(ISO_STEP_DONE, r.kind);
    TEST_ASSERT_EQUAL_INT(ISO_STATE_READY, entry()->state);
    TEST_ASSERT_EQUAL_UINT32(4096, iso_extent_table_extent_count(LAST_TABLE));
}

void test_extent_count_over_the_limit_is_too_fragmented_by_the_table_build(void) {
    /* パーツ 1025 ブロック = 4100 セクタ。1 セクタずつ 4100 本: 作業領域(4112)には入るが、4096 を超える */
    IFS.get_extents = frag_get_extents;
    g_frag_n = 4100;
    upload_all(1025ull * 2048, 1025);
    iso_step_result_t r = finish_all(0);
    TEST_ASSERT_EQUAL_INT(ISO_STEP_DONE, r.kind);
    expect_invalid(ISO_INVALID_TOO_FRAGMENTED);
}

void test_get_extents_too_fragmented_is_too_fragmented(void) {
    upload_all(SZ, PB_SMALL);
    TEST_ASSERT_TRUE(mock_fs_fail_extents(&FS, "/iso/11110001/p001", ISO_ERR_TOO_FRAGMENTED));
    iso_step_result_t r = finish_all(0);
    TEST_ASSERT_EQUAL_INT(ISO_STEP_DONE, r.kind);
    expect_invalid(ISO_INVALID_TOO_FRAGMENTED);
}

void test_extent_pool_exhaustion_is_too_fragmented(void) {
    /* 作業領域は 4096 + 16 = 4112 本。p000 が 4112 本を使い切ると、p001 の get_extents は max = 0 で TOO_FRAGMENTED を返す */
    IFS.get_extents = frag_get_extents;
    g_frag_n = 4112;
    upload_all(2 * 1030ull * 2048, 1030); /* 2 パーツ(p000 は 4120 セクタ) */
    iso_step_result_t r = finish_all(0);
    TEST_ASSERT_EQUAL_INT(ISO_STEP_DONE, r.kind);
    expect_invalid(ISO_INVALID_TOO_FRAGMENTED);
}

void test_missing_part_is_part_mismatch_with_streamed_sha(void) {
    upload_all(SZ, PB_SMALL);
    TEST_ASSERT_EQUAL_INT(ISO_OK, IFS.remove(&FS, "/iso/11110001/p001")); /* 満杯で閉じているパーツ */
    iso_step_result_t r = finish_all(0);
    TEST_ASSERT_EQUAL_INT(ISO_STEP_DONE, r.kind);
    expect_invalid(ISO_INVALID_PART_MISMATCH);
}

void test_finish_begin_after_a_part_vanished_is_size_mismatch(void) {
    upload_all(SZ, PB_SMALL);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_upload_idle_close(&UP, ID));
    reboot();
    TEST_ASSERT_EQUAL_UINT64(SZ, entry()->received);
    TEST_ASSERT_EQUAL_INT(ISO_OK, IFS.remove(&FS, "/iso/11110001/p001"));
    uint64_t rec = 0;
    TEST_ASSERT_EQUAL_INT(ISO_RES_SIZE_MISMATCH, iso_upload_finish_begin(&UP, ID, &rec)); /* 受付が、ディスクから決め直す */
    TEST_ASSERT_EQUAL_UINT64(34816, rec);
}

void test_missing_part_found_by_the_hash_step(void) {
    upload_all(SZ, PB_SMALL);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_upload_idle_close(&UP, ID));
    reboot();
    TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_upload_finish_begin(&UP, ID, NULL));
    TEST_ASSERT_EQUAL_INT(ISO_OK, IFS.remove(&FS, "/iso/11110001/p001")); /* 受付の後に消えた */
    iso_finish_job_t job;
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_finish_job_init(&job, ID));
    iso_step_result_t r = run_finish(&job, 0, NULL);
    TEST_ASSERT_EQUAL_INT(ISO_STEP_DONE, r.kind);
    expect_invalid(ISO_INVALID_PART_MISMATCH);
}

void test_short_part_is_part_mismatch_in_both_paths(void) {
    upload_all(SZ, PB_SMALL);
    TEST_ASSERT_TRUE(mock_fs_set_size(&FS, "/iso/11110001/p001", 34000));
    iso_step_result_t r = finish_all(0);
    TEST_ASSERT_EQUAL_INT(ISO_STEP_DONE, r.kind);
    expect_invalid(ISO_INVALID_PART_MISMATCH);
}

void test_short_part_found_while_rehashing(void) {
    upload_all(SZ, PB_SMALL);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_upload_idle_close(&UP, ID));
    reboot();
    TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_upload_finish_begin(&UP, ID, NULL));
    TEST_ASSERT_TRUE(mock_fs_set_size(&FS, "/iso/11110001/p001", 34000));
    iso_finish_job_t job;
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_finish_job_init(&job, ID));
    iso_step_result_t r = run_finish(&job, 0, NULL);
    TEST_ASSERT_EQUAL_INT(ISO_STEP_DONE, r.kind);
    expect_invalid(ISO_INVALID_PART_MISMATCH);
}

void test_long_part_is_part_mismatch(void) {
    upload_all(SZ, PB_SMALL);
    TEST_ASSERT_TRUE(mock_fs_set_size(&FS, "/iso/11110001/p001", 34817)); /* 期待より 1 バイト長い */
    iso_step_result_t r = finish_all(0);
    TEST_ASSERT_EQUAL_INT(ISO_STEP_DONE, r.kind);
    expect_invalid(ISO_INVALID_PART_MISMATCH);
}

void test_part_count_in_meta_that_disagrees_with_the_size_is_part_mismatch(void) {
    upload_all(SZ, PB_SMALL);
    iso_catalog_entry_t *e = entry();
    iso_meta_t m = e->meta;
    m.part_count = 2; /* 実際は 3 */
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_catalog_write_meta(&CAT, e, &m));
    iso_step_result_t r = finish_all(0);
    TEST_ASSERT_EQUAL_INT(ISO_STEP_DONE, r.kind);
    expect_invalid(ISO_INVALID_PART_MISMATCH);
}

/* ---- I/O エラー(incomplete。再試行は finish) ----------------------------------------------------- */

void test_get_extents_io_error_makes_incomplete_and_a_retry_succeeds(void) {
    upload_all(SZ, PB_SMALL);
    TEST_ASSERT_TRUE(mock_fs_fail_extents(&FS, "/iso/11110001/p001", ISO_ERR_IO));
    iso_step_result_t r = finish_all(0);
    TEST_ASSERT_EQUAL_INT(ISO_STEP_ERROR, r.kind);
    TEST_ASSERT_EQUAL_INT(ISO_ERR_IO, r.err);
    expect_incomplete_after_ioerr();
    expect_meta(ISO_META_INCOMPLETE, 0);
    TEST_ASSERT_EQUAL_UINT64(SZ, entry()->received); /* 実ファイルの大きさから決めた */
    TEST_ASSERT_TRUE(mock_fs_fail_extents(&FS, "/iso/11110001/p001", 0));
    FS.n_read_at = 0;
    r = finish_all(0);
    TEST_ASSERT_EQUAL_INT(ISO_STEP_DONE, r.kind);
    expect_ready(SHA_100000, 3);
    TEST_ASSERT_TRUE(FS.n_read_at > 1); /* 逐次の SHA-256 は確定(final)で使い切ったので、再試行は読み直す */
}

void test_stat_io_error_during_index(void) {
    upload_all(SZ, PB_SMALL);
    iso_finish_job_t job;
    TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_upload_finish_begin(&UP, ID, NULL));
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_finish_job_init(&job, ID));
    iso_step_result_t r;
    /* インデックスの段階まで進めてから、stat を失敗させる */
    do {
        r = iso_upload_finish_step(&UP, &job, 0, false);
    } while (r.kind == ISO_STEP_CONTINUE && job.idx.phase == 0);
    mock_fs_fail_path(&FS, "/iso/11110001/p000");
    do {
        r = iso_upload_finish_step(&UP, &job, 0, false);
    } while (r.kind == ISO_STEP_CONTINUE);
    mock_fs_fail_path(&FS, NULL);
    TEST_ASSERT_EQUAL_INT(ISO_STEP_ERROR, r.kind);
    TEST_ASSERT_EQUAL_INT(ISO_ERR_IO, r.err);
    expect_incomplete_after_ioerr();
}

void test_close_failure_keeps_the_streamed_sha_for_the_retry(void) {
    upload_all(SZ, PB_SMALL);
    TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_upload_finish_begin(&UP, ID, NULL));
    fx_settle();
    mock_fs_error_at(&FS, 1, ISO_ERR_IO); /* 最初の書き込み = 現在のパーツの sync */
    iso_finish_job_t job;
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_finish_job_init(&job, ID));
    iso_step_result_t r = run_finish(&job, 0, NULL);
    TEST_ASSERT_EQUAL_INT(ISO_STEP_ERROR, r.kind);
    TEST_ASSERT_EQUAL_INT(ISO_ERR_IO, r.err);
    expect_incomplete_after_ioerr();
    TEST_ASSERT_EQUAL_UINT64(SZ, entry()->received); /* close が同期した */
    mock_fs_reset_counter(&FS);
    FS.n_read_at = 0;
    r = finish_all(0);
    TEST_ASSERT_EQUAL_INT(ISO_STEP_DONE, r.kind);
    expect_ready(SHA_100000, 3);
    TEST_ASSERT_EQUAL_INT(1, FS.n_read_at);
}

void test_hash_read_error_makes_incomplete(void) {
    upload_all(SZ, PB_SMALL);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_upload_idle_close(&UP, ID));
    reboot();
    mock_fs_fail_read(&FS, "/iso/11110001/p001", 2);
    iso_step_result_t r = finish_all(0);
    TEST_ASSERT_EQUAL_INT(ISO_STEP_ERROR, r.kind);
    TEST_ASSERT_EQUAL_INT(ISO_ERR_IO, r.err);
    expect_incomplete_after_ioerr();
    mock_fs_fail_read(&FS, NULL, 0);
    r = finish_all(0);
    TEST_ASSERT_EQUAL_INT(ISO_STEP_DONE, r.kind);
    expect_ready(SHA_100000, 3);
}

void test_hash_open_error_makes_incomplete(void) {
    upload_all(SZ, PB_SMALL);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_upload_idle_close(&UP, ID));
    reboot();
    TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_upload_finish_begin(&UP, ID, NULL));
    mock_fs_fail_open(&FS, "/iso/11110001/p002");
    iso_finish_job_t job;
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_finish_job_init(&job, ID));
    iso_step_result_t r = run_finish(&job, 0, NULL);
    TEST_ASSERT_EQUAL_INT(ISO_STEP_ERROR, r.kind);
    expect_incomplete_after_ioerr();
    mock_fs_fail_open(&FS, NULL);
}

void test_signature_read_error_makes_incomplete(void) {
    upload_all(SZ, PB_SMALL);
    mock_fs_fail_read(&FS, "/iso/11110001/p000", 1); /* 逐次の SHA-256 なので、最初の読み出し = 署名 */
    iso_step_result_t r = finish_all(0);
    TEST_ASSERT_EQUAL_INT(ISO_STEP_ERROR, r.kind);
    TEST_ASSERT_EQUAL_INT(ISO_ERR_IO, r.err);
    expect_incomplete_after_ioerr();
    mock_fs_fail_read(&FS, NULL, 0);
}

void test_signature_open_error_makes_incomplete(void) {
    upload_all(SZ, PB_SMALL);
    mock_fs_fail_open(&FS, "/iso/11110001/p000");
    iso_step_result_t r = finish_all(0);
    TEST_ASSERT_EQUAL_INT(ISO_STEP_ERROR, r.kind);
    expect_incomplete_after_ioerr();
    mock_fs_fail_open(&FS, NULL);
}

void test_allocation_failure_of_the_work_area_makes_incomplete(void) {
    upload_all(SZ, PB_SMALL);
    ALLOC_CALLS = 0;
    ALLOC_FAIL_AT = 1;
    iso_step_result_t r = finish_all(0);
    TEST_ASSERT_EQUAL_INT(ISO_STEP_ERROR, r.kind);
    TEST_ASSERT_EQUAL_INT(ISO_ERR_NOMEM, r.err);
    expect_incomplete_after_ioerr();
    ALLOC_FAIL_AT = 0;
    r = finish_all(0);
    TEST_ASSERT_EQUAL_INT(ISO_STEP_DONE, r.kind);
}

void test_allocation_failure_of_the_table_makes_incomplete_and_frees_the_work_area(void) {
    upload_all(SZ, PB_SMALL);
    ALLOC_CALLS = 0;
    ALLOC_FAIL_AT = 2; /* 作業領域(1)は成功、表の確保(2)が失敗 */
    iso_step_result_t r = finish_all(0);
    TEST_ASSERT_EQUAL_INT(ISO_STEP_ERROR, r.kind);
    TEST_ASSERT_EQUAL_INT(ISO_ERR_NOMEM, r.err);
    expect_incomplete_after_ioerr(); /* ALLOC_LIVE == 0: 作業領域を解放した */
}

void test_meta_write_failure_at_the_end_does_not_install_the_table(void) {
    upload_all(SZ, PB_SMALL);
    TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_upload_finish_begin(&UP, ID, NULL));
    iso_finish_job_t job;
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_finish_job_init(&job, ID));
    iso_step_result_t r;
    do {
        r = iso_upload_finish_step(&UP, &job, 0, false);
    } while (r.kind == ISO_STEP_CONTINUE && job.phase != 6); /* FP_META の手前まで */
    TEST_ASSERT_EQUAL_INT(ISO_STEP_CONTINUE, r.kind);
    fx_settle();
    mock_fs_error_at(&FS, 1, ISO_ERR_IO); /* ready の meta の書き込み */
    r = iso_upload_finish_step(&UP, &job, 0, false);
    TEST_ASSERT_EQUAL_INT(ISO_STEP_ERROR, r.kind);
    expect_incomplete_after_ioerr();
    expect_meta(ISO_META_INCOMPLETE, 0); /* ready の書き込みは失敗、incomplete の書き込みは成功(注入は 1 回だけ) */
}

void test_invalid_meta_write_failure_returns_error_but_state_is_invalid(void) {
    SIG = "XXXXX";
    upload_all(SZ, PB_SMALL);
    TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_upload_finish_begin(&UP, ID, NULL));
    iso_finish_job_t job;
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_finish_job_init(&job, ID));
    iso_step_result_t r;
    do {
        r = iso_upload_finish_step(&UP, &job, 0, false);
    } while (r.kind == ISO_STEP_CONTINUE && job.phase < 4); /* FP_SIG の手前まで */
    fx_settle();
    mock_fs_error_at(&FS, 1, ISO_ERR_IO);
    r = iso_upload_finish_step(&UP, &job, 0, false);
    TEST_ASSERT_EQUAL_INT(ISO_STEP_ERROR, r.kind);
    TEST_ASSERT_EQUAL_INT(ISO_STATE_INVALID, entry()->state);
    TEST_ASSERT_EQUAL_UINT8(ISO_INVALID_NOT_ISO, entry()->invalid_reason);
    TEST_ASSERT_FALSE(iso_upload_slot_busy(&UP));
}

/* ---- 中止、古いジョブ ------------------------------------------------------------------------ */

void test_aborted_job_frees_its_work_area_and_changes_nothing(void) {
    upload_all(SZ, PB_SMALL);
    TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_upload_finish_begin(&UP, ID, NULL));
    iso_finish_job_t job;
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_finish_job_init(&job, ID));
    iso_step_result_t r;
    do {
        r = iso_upload_finish_step(&UP, &job, 0, false);
    } while (r.kind == ISO_STEP_CONTINUE && job.idx.pool == NULL); /* 作業領域を確保した後まで */
    TEST_ASSERT_NOT_NULL(job.idx.pool);
    TEST_ASSERT_EQUAL_INT(1, ALLOC_LIVE);
    fx_settle();
    r = iso_upload_finish_step(&UP, &job, 0, true);
    TEST_ASSERT_EQUAL_INT(ISO_STEP_DONE, r.kind);
    TEST_ASSERT_EQUAL_INT(0, ALLOC_LIVE);
    TEST_ASSERT_EQUAL_INT(0, FS.nlog); /* 何も書かない */
    TEST_ASSERT_EQUAL_INT(ISO_STATE_FINALIZING, entry()->state); /* 状態は DELETE のイベントが決める。ここでは変えない */
    TEST_ASSERT_EQUAL_INT(0, TABLES_READY);
    /* 中止の後は、何度呼んでも DONE */
    r = iso_upload_finish_step(&UP, &job, 0, false);
    TEST_ASSERT_EQUAL_INT(ISO_STEP_DONE, r.kind);
}

void test_stale_job_whose_image_is_no_longer_finalizing_is_done_without_effects(void) {
    upload_all(SZ, PB_SMALL);
    TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_upload_finish_begin(&UP, ID, NULL));
    iso_finish_job_t job;
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_finish_job_init(&job, ID));
    iso_upload_abort(&UP, ID); /* 削除で枠が空いた */
    entry()->state = ISO_STATE_DELETING;
    fx_settle();
    iso_step_result_t r = iso_upload_finish_step(&UP, &job, 0, false);
    TEST_ASSERT_EQUAL_INT(ISO_STEP_DONE, r.kind);
    TEST_ASSERT_EQUAL_INT(0, FS.nlog);
    TEST_ASSERT_EQUAL_INT(ISO_STATE_DELETING, entry()->state);
}

void test_stale_job_when_the_slot_belongs_to_nobody(void) {
    upload_all(SZ, PB_SMALL);
    TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_upload_finish_begin(&UP, ID, NULL));
    iso_finish_job_t job;
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_finish_job_init(&job, ID));
    iso_upload_reinit(&UP); /* SD の再初期化: 枠が空き、incomplete になる */
    fx_settle();
    iso_step_result_t r = iso_upload_finish_step(&UP, &job, 0, false);
    TEST_ASSERT_EQUAL_INT(ISO_STEP_DONE, r.kind);
    TEST_ASSERT_EQUAL_INT(0, FS.nlog);
    TEST_ASSERT_EQUAL_INT(ISO_STATE_INCOMPLETE, entry()->state);
}

void test_table_epoch_is_read_when_the_indexing_starts(void) {
    upload_all(SZ, PB_SMALL);
    TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_upload_finish_begin(&UP, ID, NULL));
    iso_finish_job_t job;
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_finish_job_init(&job, ID));
    iso_step_result_t r;
    do {
        r = iso_upload_finish_step(&UP, &job, 0, false);
    } while (r.kind == ISO_STEP_CONTINUE && job.idx.pool == NULL);
    EPOCH = 99; /* 途中でカードが替わった */
    while (r.kind == ISO_STEP_CONTINUE) r = iso_upload_finish_step(&UP, &job, 0, false);
    TEST_ASSERT_EQUAL_INT(ISO_STEP_DONE, r.kind);
    TEST_ASSERT_EQUAL_UINT32(7, iso_extent_table_epoch(LAST_TABLE)); /* 開始時の値(古いカードの表になる) */
}

void test_table_hook_may_be_absent(void) {
    iso_upload_cfg_t cfg;
    fx_up_cfg(&cfg, 0, 0);
    cfg.hooks.table_ready = NULL;
    cfg.hooks.view_changed = NULL;
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_upload_init(&UP, &cfg));
    iso_upload_set_free(&UP, 64 * GIB, 60 * GIB);
    upload_all(SZ, PB_SMALL);
    iso_step_result_t r = finish_all(0);
    TEST_ASSERT_EQUAL_INT(ISO_STEP_DONE, r.kind);
    TEST_ASSERT_EQUAL_INT(ISO_STATE_READY, entry()->state);
    TEST_ASSERT_EQUAL_INT(0, ALLOC_LIVE); /* 受け取らない表は、放した */
}


/* ---- 追加: 通知、資源、境界 ------------------------------------------------------------------ */

void test_a_new_upload_after_an_incomplete_one_starts_a_fresh_sha_state(void) {
    /* 1 つ目: 途中でアイドル(SHA-256 の状態は、そのイメージのものとして残る) */
    char id1[9];
    img_begin_pb("first", SZ, PB_SMALL, NULL, id1);
    put_all(id1, 0, 5000, 5000);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_upload_idle_close(&UP, id1));
    TEST_ASSERT_EQUAL_UINT64(5000, UP.sha_bytes);
    /* 2 つ目: begin で SHA-256 を初期化し直す。逐次の計算が使える(読み直さない) */
    img_begin_pb("second", SZ, PB_SMALL, NULL, ID);
    TEST_ASSERT_EQUAL_STRING("11110002", ID);
    TEST_ASSERT_EQUAL_UINT64(0, UP.sha_bytes);
    put_all(ID, 0, SZ, 20000);
    FS.n_read_at = 0;
    iso_step_result_t r = finish_all(0);
    TEST_ASSERT_EQUAL_INT(ISO_STEP_DONE, r.kind);
    expect_ready(SHA_100000, 3);
    TEST_ASSERT_EQUAL_INT(1, FS.n_read_at);
}

void test_finish_notifies_the_view_at_the_acceptance_and_at_the_end_only(void) {
    upload_all(SZ, PB_SMALL);
    int v = VIEW_CALLS;
    TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_upload_finish_begin(&UP, ID, NULL));
    TEST_ASSERT_EQUAL_INT(v + 1, VIEW_CALLS);
    iso_finish_job_t job;
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_finish_job_init(&job, ID));
    iso_step_result_t r = run_finish(&job, 0, NULL);
    TEST_ASSERT_EQUAL_INT(ISO_STEP_DONE, r.kind);
    TEST_ASSERT_EQUAL_INT(v + 2, VIEW_CALLS); /* 途中のステップでは更新しない。ready になったときに 1 回 */
}

void test_invalid_end_notifies_and_refreshes_the_free_space(void) {
    SIG = "XXXXX";
    upload_all(SZ, PB_SMALL);
    mock_fs_set_mount_info(&FS, 64 * GIB, 51 * GIB, 32768);
    int v = VIEW_CALLS;
    TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_upload_finish_begin(&UP, ID, NULL));
    iso_finish_job_t job;
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_finish_job_init(&job, ID));
    iso_step_result_t r = run_finish(&job, 0, NULL);
    TEST_ASSERT_EQUAL_INT(ISO_STEP_DONE, r.kind);
    expect_invalid(ISO_INVALID_NOT_ISO);
    TEST_ASSERT_EQUAL_INT(v + 2, VIEW_CALLS);
    TEST_ASSERT_EQUAL_UINT64(51 * GIB, UP.free_bytes); /* invalid でも、finish の完了で採り直す */
    TEST_ASSERT_EQUAL_UINT64(0, UP.consumed_since);
}

void test_ioerr_end_notifies_and_takes_received_from_the_disk(void) {
    upload_all(SZ, PB_SMALL);
    TEST_ASSERT_TRUE(mock_fs_fail_extents(&FS, "/iso/11110001/p001", ISO_ERR_IO));
    TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_upload_finish_begin(&UP, ID, NULL));
    iso_finish_job_t job;
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_finish_job_init(&job, ID));
    iso_step_result_t r;
    do {
        r = iso_upload_finish_step(&UP, &job, 0, false);
    } while (r.kind == ISO_STEP_CONTINUE && job.idx.pool == NULL); /* ファイルを閉じた後まで */
    TEST_ASSERT_EQUAL_INT(ISO_OK, IFS.remove(&FS, "/iso/11110001/p002"));
    TEST_ASSERT_TRUE(mock_fs_set_size(&FS, "/iso/11110001/p001", 34000)); /* p000 と、p001 の一部だけ残る */
    int v = VIEW_CALLS;
    while (r.kind == ISO_STEP_CONTINUE) r = iso_upload_finish_step(&UP, &job, 0, false);
    TEST_ASSERT_EQUAL_INT(ISO_STEP_ERROR, r.kind);
    TEST_ASSERT_EQUAL_INT(v + 1, VIEW_CALLS);
    expect_incomplete_after_ioerr();
    TEST_ASSERT_EQUAL_UINT64(34816 + 34000, entry()->received); /* 実ファイルの大きさから決める */
}

void test_missing_first_part_with_the_streamed_sha_is_part_mismatch(void) {
    upload_all(SZ, PB_SMALL);
    TEST_ASSERT_EQUAL_INT(ISO_OK, IFS.remove(&FS, "/iso/11110001/p000")); /* 署名の読み出しが NOT_FOUND */
    iso_step_result_t r = finish_all(0);
    TEST_ASSERT_EQUAL_INT(ISO_STEP_DONE, r.kind);
    expect_invalid(ISO_INVALID_PART_MISMATCH);
}

void test_close_failure_after_a_good_sync_is_an_io_error(void) {
    upload_all(SZ, PB_SMALL);
    TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_upload_finish_begin(&UP, ID, NULL));
    mock_fs_set_fail_close(&FS, true);
    iso_finish_job_t job;
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_finish_job_init(&job, ID));
    iso_step_result_t r = run_finish(&job, 0, NULL);
    mock_fs_set_fail_close(&FS, false);
    TEST_ASSERT_EQUAL_INT(ISO_STEP_ERROR, r.kind);
    TEST_ASSERT_EQUAL_INT(ISO_ERR_IO, r.err);
    expect_incomplete_after_ioerr();
}

void test_hash_step_of_an_unknown_image_fails_without_touching_the_sha_state(void) {
    iso_finish_job_t job;
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_finish_job_init(&job, "0000000b"));
    iso_step_result_t r = iso_upload_hash_step(&UP, &job, 0);
    TEST_ASSERT_EQUAL_INT(ISO_STEP_DONE, r.kind);
    TEST_ASSERT_EQUAL_INT(ISO_OUTCOME_IOERR, job.fail);
    TEST_ASSERT_EQUAL_INT(ISO_ERR_NOT_FOUND, job.fail_err);
    TEST_ASSERT_EQUAL_UINT8(0, job.fail_reason);
    TEST_ASSERT_FALSE(job.have_digest);
    TEST_ASSERT_FALSE(job.hash_started);
}

void test_hash_step_discards_the_streamed_state_it_overwrites(void) {
    upload_all(SZ, PB_SMALL);
    TEST_ASSERT_TRUE(UP.sha_valid);
    iso_finish_job_t job;
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_finish_job_init(&job, ID));
    TEST_ASSERT_EQUAL_INT(ISO_OK, IFS.sync(&FS, UP.f)); /* 読み取りで開けるように、書き込み用のハンドルを閉じる */
    TEST_ASSERT_EQUAL_INT(ISO_OK, IFS.close(&FS, UP.f));
    UP.f = NULL;
    iso_step_result_t r = iso_upload_hash_step(&UP, &job, 0);
    TEST_ASSERT_EQUAL_INT(ISO_STEP_CONTINUE, r.kind);
    TEST_ASSERT_FALSE(UP.sha_valid); /* 作業用に同じ領域を使うので、逐次の状態は使えなくなる */
}

void test_stale_job_after_the_work_area_was_allocated_frees_it(void) {
    upload_all(SZ, PB_SMALL);
    TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_upload_finish_begin(&UP, ID, NULL));
    iso_finish_job_t job;
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_finish_job_init(&job, ID));
    iso_step_result_t r;
    do {
        r = iso_upload_finish_step(&UP, &job, 0, false);
    } while (r.kind == ISO_STEP_CONTINUE && job.idx.pool == NULL);
    TEST_ASSERT_EQUAL_INT(1, ALLOC_LIVE);
    iso_upload_reinit(&UP); /* SD の再初期化: incomplete になり、枠が空く */
    r = iso_upload_finish_step(&UP, &job, 0, false);
    TEST_ASSERT_EQUAL_INT(ISO_STEP_DONE, r.kind);
    TEST_ASSERT_EQUAL_INT(0, ALLOC_LIVE); /* 古いジョブの作業領域を解放する */
    r = iso_upload_finish_step(&UP, &job, 0, false);
    TEST_ASSERT_EQUAL_INT(ISO_STEP_DONE, r.kind);
}

void test_undo_and_acceptance_notify_the_view(void) {
    upload_all(SZ, PB_SMALL);
    iso_catalog_entry_t *e = entry();
    int v = VIEW_CALLS;
    TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_upload_finish_begin(&UP, ID, NULL));
    TEST_ASSERT_EQUAL_INT(v + 1, VIEW_CALLS);
    iso_upload_finish_undo(&UP, e, ISO_STATE_UPLOADING);
    TEST_ASSERT_EQUAL_INT(v + 2, VIEW_CALLS);
}

/* 古いジョブの判定: どの条件が欠けても、何もせずに DONE(資源は解放) */
static void stale_case_setup(iso_finish_job_t *job) {
    upload_all(SZ, PB_SMALL);
    TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_upload_finish_begin(&UP, ID, NULL));
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_finish_job_init(job, ID));
    fx_settle();
}

static void expect_stale(iso_finish_job_t *job) {
    iso_step_result_t r = iso_upload_finish_step(&UP, job, 0, false);
    TEST_ASSERT_EQUAL_INT(ISO_STEP_DONE, r.kind);
    TEST_ASSERT_EQUAL_INT(0, FS.nlog);
    TEST_ASSERT_EQUAL_INT(0, TABLES_READY);
}

void test_stale_job_when_the_image_left_the_catalog(void) {
    iso_finish_job_t job;
    stale_case_setup(&job);
    iso_upload_abort(&UP, ID);
    iso_catalog_remove(&CAT, ID);
    fx_settle();
    expect_stale(&job);
}

void test_stale_job_when_the_state_changed_but_the_slot_is_still_held(void) {
    iso_finish_job_t job;
    stale_case_setup(&job);
    entry()->state = ISO_STATE_INVALID; /* 枠は、finalizing のまま */
    expect_stale(&job);
    entry()->state = ISO_STATE_FINALIZING;
}

void test_stale_job_when_the_slot_is_not_finalizing(void) {
    iso_finish_job_t job;
    stale_case_setup(&job);
    UP.slot_finalizing = false;
    expect_stale(&job);
    UP.slot_finalizing = true;
}

void test_stale_job_when_the_slot_belongs_to_another_image(void) {
    iso_finish_job_t job;
    stale_case_setup(&job);
    snprintf(UP.slot_id, sizeof(UP.slot_id), "deadbeef");
    expect_stale(&job);
    snprintf(UP.slot_id, sizeof(UP.slot_id), "%s", ID);
}

void test_stale_job_when_the_meta_is_invalid(void) {
    iso_finish_job_t job;
    stale_case_setup(&job);
    entry()->meta_valid = false;
    expect_stale(&job);
    entry()->meta_valid = true;
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_finish_begin_complete_upload_becomes_finalizing_and_keeps_the_slot);
    RUN_TEST(test_finish_begin_size_mismatch_reports_received);
    RUN_TEST(test_finish_begin_incomplete_with_less_than_size_is_size_mismatch);
    RUN_TEST(test_finish_begin_incomplete_complete_takes_the_free_slot);
    RUN_TEST(test_finish_begin_incomplete_complete_with_the_slot_used_is_upload_in_progress);
    RUN_TEST(test_finish_begin_rereads_received_for_an_incomplete_image);
    RUN_TEST(test_finish_begin_other_states);
    RUN_TEST(test_finish_begin_reload_error_changes_nothing);
    RUN_TEST(test_finish_undo_restores_the_state_and_the_slot);
    RUN_TEST(test_finish_with_the_streamed_sha_reaches_ready);
    RUN_TEST(test_finish_refreshes_the_free_space_cache);
    RUN_TEST(test_finish_with_a_matching_declared_sha_reaches_ready_and_stores_the_computed_flag);
    RUN_TEST(test_finish_after_a_reboot_recomputes_the_sha_in_budget_steps);
    RUN_TEST(test_finish_after_a_reboot_with_a_larger_budget_makes_fewer_steps);
    RUN_TEST(test_hash_step_budget_zero_still_progresses);
    RUN_TEST(test_hash_step_runs_to_the_digest_across_parts);
    RUN_TEST(test_resumed_upload_after_idle_still_streams_the_sha);
    RUN_TEST(test_finish_after_a_failed_and_rolled_back_chunk_still_streams);
    RUN_TEST(test_finish_after_an_unrolled_partial_write_recomputes_the_sha);
    RUN_TEST(test_minimal_image_34816_bytes_one_part);
    RUN_TEST(test_finish_step_sequence_closes_before_reading);
    RUN_TEST(test_signature_bea01_is_accepted);
    RUN_TEST(test_signature_in_a_later_part_when_parts_are_small);
    RUN_TEST(test_wrong_signature_is_not_iso);
    RUN_TEST(test_declared_sha_mismatch_with_streamed_state);
    RUN_TEST(test_declared_sha_mismatch_in_the_first_byte);
    RUN_TEST(test_declared_sha_mismatch_with_recomputed_digest);
    RUN_TEST(test_declared_sha_matches_after_a_reboot);
    RUN_TEST(test_undeclared_sha_has_no_comparison);
    RUN_TEST(test_invalid_image_stays_invalid_for_later_finish);
    RUN_TEST(test_extent_count_at_the_limit_is_accepted);
    RUN_TEST(test_extent_count_over_the_limit_is_too_fragmented_by_the_table_build);
    RUN_TEST(test_get_extents_too_fragmented_is_too_fragmented);
    RUN_TEST(test_extent_pool_exhaustion_is_too_fragmented);
    RUN_TEST(test_missing_part_is_part_mismatch_with_streamed_sha);
    RUN_TEST(test_finish_begin_after_a_part_vanished_is_size_mismatch);
    RUN_TEST(test_missing_part_found_by_the_hash_step);
    RUN_TEST(test_short_part_is_part_mismatch_in_both_paths);
    RUN_TEST(test_short_part_found_while_rehashing);
    RUN_TEST(test_long_part_is_part_mismatch);
    RUN_TEST(test_part_count_in_meta_that_disagrees_with_the_size_is_part_mismatch);
    RUN_TEST(test_get_extents_io_error_makes_incomplete_and_a_retry_succeeds);
    RUN_TEST(test_stat_io_error_during_index);
    RUN_TEST(test_close_failure_keeps_the_streamed_sha_for_the_retry);
    RUN_TEST(test_hash_read_error_makes_incomplete);
    RUN_TEST(test_hash_open_error_makes_incomplete);
    RUN_TEST(test_signature_read_error_makes_incomplete);
    RUN_TEST(test_signature_open_error_makes_incomplete);
    RUN_TEST(test_allocation_failure_of_the_work_area_makes_incomplete);
    RUN_TEST(test_allocation_failure_of_the_table_makes_incomplete_and_frees_the_work_area);
    RUN_TEST(test_meta_write_failure_at_the_end_does_not_install_the_table);
    RUN_TEST(test_invalid_meta_write_failure_returns_error_but_state_is_invalid);
    RUN_TEST(test_aborted_job_frees_its_work_area_and_changes_nothing);
    RUN_TEST(test_stale_job_whose_image_is_no_longer_finalizing_is_done_without_effects);
    RUN_TEST(test_stale_job_when_the_slot_belongs_to_nobody);
    RUN_TEST(test_table_epoch_is_read_when_the_indexing_starts);
    RUN_TEST(test_table_hook_may_be_absent);
    RUN_TEST(test_a_new_upload_after_an_incomplete_one_starts_a_fresh_sha_state);
    RUN_TEST(test_finish_notifies_the_view_at_the_acceptance_and_at_the_end_only);
    RUN_TEST(test_invalid_end_notifies_and_refreshes_the_free_space);
    RUN_TEST(test_ioerr_end_notifies_and_takes_received_from_the_disk);
    RUN_TEST(test_missing_first_part_with_the_streamed_sha_is_part_mismatch);
    RUN_TEST(test_close_failure_after_a_good_sync_is_an_io_error);
    RUN_TEST(test_hash_step_of_an_unknown_image_fails_without_touching_the_sha_state);
    RUN_TEST(test_hash_step_discards_the_streamed_state_it_overwrites);
    RUN_TEST(test_stale_job_after_the_work_area_was_allocated_frees_it);
    RUN_TEST(test_undo_and_acceptance_notify_the_view);
    RUN_TEST(test_stale_job_when_the_image_left_the_catalog);
    RUN_TEST(test_stale_job_when_the_state_changed_but_the_slot_is_still_held);
    RUN_TEST(test_stale_job_when_the_slot_is_not_finalizing);
    RUN_TEST(test_stale_job_when_the_slot_belongs_to_another_image);
    RUN_TEST(test_stale_job_when_the_meta_is_invalid);
    return UNITY_END();
}
