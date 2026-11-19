/*
 * iso_core のジョブのステップ(iso_job_step)と、公開ビュー(hooks.view_update): FINISH / INDEX / IDLE_CLOSE / DELETE の遷移、ERROR のときの状態
 * (設計 3.4 の表)、DELETE の参照待ち(30 秒の上限、時刻の巻き戻り)、free_bytes の更新、ビューの内容。期待値は手書き。
 */
#include "core_common.h"

void setUp(void) { fx_core_setup(0, 0); }
void tearDown(void) { fx_core_teardown(); }

#define SZ 100000ull

static iso_upload_t *up(void) { return iso_core_upload(&CORE); }
static void scan(void) { TEST_ASSERT_EQUAL_INT(ISO_OK, iso_catalog_scan(&CAT)); }
static iso_catalog_entry_t *find(const char *id) { return iso_catalog_find(&CAT, id); }

static const iso_view_image_t *view_of(const char *id) {
    for (size_t i = 0; i < VIEW_COPY.count; i++) {
        if (strcmp(VIEW_COPY.images[i].id, id) == 0) return &VIEW_COPY.images[i];
    }
    return NULL;
}

static void expect_meta_state(const char *id, uint8_t state, uint8_t reason) {
    iso_slots_t s;
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_slots_read(&IFS, id, &s));
    TEST_ASSERT_TRUE(s.adopted >= 0);
    TEST_ASSERT_EQUAL_UINT8(state, s.meta[s.adopted].state);
    TEST_ASSERT_EQUAL_UINT8(reason, s.meta[s.adopted].invalid_reason);
}

/* アップロードを始めて(パーツ 17 ブロック)、全部送る */
static void upload_image(char id[ISO_ID_LEN + 1], uint64_t size) {
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_upload_begin(up(), "live", 4, size, NULL, id));
    iso_catalog_entry_t *e = find(id);
    iso_meta_t m = e->meta;
    m.part_blocks = PB_SMALL;
    m.part_count = (uint16_t)iso_extent_part_count(size, PB_SMALL);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_catalog_write_meta(&CAT, e, &m));
    put_all(id, 0, size, 20000);
}

/* ---- FINISH ------------------------------------------------------------------------------- */

void test_finish_job_end_to_end(void) {
    char id[9];
    upload_image(id, SZ);
    mock_fs_set_mount_info(&FS, 64 * GIB, 55 * GIB, 32768);
    uint64_t rec = 0;
    TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_core_submit_finish(&CORE, id, &rec));
    TEST_ASSERT_EQUAL_UINT64(SZ, rec);
    TEST_ASSERT_EQUAL_INT(ISO_STATE_FINALIZING, find(id)->state);
    int before = VIEW_UPDATES;
    TEST_ASSERT_EQUAL_INT(1, drive(4096, 1000));
    TEST_ASSERT_EQUAL_INT(ISO_JOB_FINISH, JOBS[0].kind);
    TEST_ASSERT_EQUAL_INT(ISO_STEP_DONE, JOBS[0].result);
    TEST_ASSERT_TRUE(VIEW_UPDATES > before); /* 状態が変わるたび(ready になった、枠を空けた、ジョブが終わった)にビューを更新する */
    TEST_ASSERT_EQUAL_INT(ISO_STATE_READY, find(id)->state);
    iso_extent_table_t *t = iso_core_table(&CORE, id);
    TEST_ASSERT_NOT_NULL(t);
    TEST_ASSERT_EQUAL_UINT32(1, iso_extent_table_refs(t)); /* カタログの分 */
    TEST_ASSERT_EQUAL_UINT32(3, iso_extent_table_part_count(t));
    const iso_view_image_t *v = view_of(id);
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_EQUAL_INT(ISO_STATE_READY, v->state);
    TEST_ASSERT_TRUE(v->table == t);
    TEST_ASSERT_TRUE(v->has_sha);
    uint8_t want[32];
    gen_sha(SZ, want);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, v->sha256, 32);
    TEST_ASSERT_EQUAL_UINT64(SZ, v->received);
    TEST_ASSERT_EQUAL_UINT64(SZ, v->size_bytes);
    TEST_ASSERT_FALSE(VIEW_COPY.upload_active);
    TEST_ASSERT_EQUAL_INT(ISO_UP_ACT_NONE, VIEW_COPY.upload_state);
    TEST_ASSERT_EQUAL_UINT64(55 * GIB, VIEW_COPY.free_bytes); /* FINISH の完了で、free_bytes を採り直す */
    TEST_ASSERT_EQUAL_UINT64(0, up()->consumed_since);
}

void test_view_in_the_middle_of_a_finish_job(void) {
    char id[9];
    upload_image(id, SZ);
    TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_core_submit_finish(&CORE, id, NULL));
    TEST_ASSERT_TRUE(VIEW_COPY.upload_active); /* submit で、ビューを更新した */
    TEST_ASSERT_EQUAL_STRING(id, VIEW_COPY.upload_id);
    TEST_ASSERT_EQUAL_INT(ISO_UP_ACT_FINALIZING, VIEW_COPY.upload_state);
    TEST_ASSERT_EQUAL_INT(ISO_STATE_FINALIZING, view_of(id)->state);
    iso_job_t *j = iso_core_pick(&CORE);
    iso_step_result_t r = iso_job_step(&CORE, j, 0);
    TEST_ASSERT_EQUAL_INT(ISO_STEP_CONTINUE, r.kind);
    TEST_ASSERT_EQUAL_INT(ISO_STATE_FINALIZING, view_of(id)->state);
    TEST_ASSERT_NULL(view_of(id)->table);
    run_job(j, 0, 1000, 1000, NULL);
}

void test_finish_job_io_error_becomes_incomplete_and_the_view_follows(void) {
    char id[9];
    upload_image(id, SZ);
    TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_core_submit_finish(&CORE, id, NULL));
    TEST_ASSERT_TRUE(mock_fs_fail_extents(&FS, "/iso/11110001/p001", ISO_ERR_IO));
    TEST_ASSERT_EQUAL_INT(1, drive(0, 1000));
    TEST_ASSERT_EQUAL_INT(ISO_STEP_ERROR, JOBS[0].result);
    TEST_ASSERT_EQUAL_INT(ISO_ERR_IO, JOBS[0].err);
    TEST_ASSERT_EQUAL_INT(ISO_STATE_INCOMPLETE, find(id)->state);
    TEST_ASSERT_EQUAL_INT(ISO_STATE_INCOMPLETE, view_of(id)->state);
    TEST_ASSERT_FALSE(VIEW_COPY.upload_active);
    TEST_ASSERT_EQUAL_UINT64(SZ, view_of(id)->received);
    TEST_ASSERT_NULL(iso_core_table(&CORE, id));
    TEST_ASSERT_NULL(iso_core_pick(&CORE)); /* ジョブは終わった(自動では再試行しない) */
    /* finish の再試行 */
    TEST_ASSERT_TRUE(mock_fs_fail_extents(&FS, "/iso/11110001/p001", 0));
    TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_core_submit_finish(&CORE, id, NULL));
    TEST_ASSERT_EQUAL_INT(1, drive(0, 1000));
    TEST_ASSERT_EQUAL_INT(ISO_STEP_DONE, JOBS[0].result);
    TEST_ASSERT_EQUAL_INT(ISO_STATE_READY, find(id)->state);
}

void test_finish_job_with_a_wrong_signature_ends_invalid_with_done(void) {
    SIG = "XXXXX";
    char id[9];
    upload_image(id, SZ);
    TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_core_submit_finish(&CORE, id, NULL));
    TEST_ASSERT_EQUAL_INT(1, drive(0, 1000));
    TEST_ASSERT_EQUAL_INT(ISO_STEP_DONE, JOBS[0].result); /* invalid は、ジョブの失敗ではない */
    TEST_ASSERT_EQUAL_INT(ISO_STATE_INVALID, view_of(id)->state);
    TEST_ASSERT_EQUAL_UINT8(ISO_INVALID_NOT_ISO, view_of(id)->invalid_reason);
    TEST_ASSERT_FALSE(view_of(id)->has_sha);
    TEST_ASSERT_NULL(iso_core_table(&CORE, id));
    expect_meta_state(id, ISO_META_INVALID, ISO_INVALID_NOT_ISO);
    TEST_ASSERT_EQUAL_INT(ISO_RES_INVALID_IMAGE, iso_core_submit_finish(&CORE, id, NULL));
}

void test_finish_of_a_ready_image_is_idempotent(void) {
    char id[9];
    upload_image(id, SZ);
    TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_core_submit_finish(&CORE, id, NULL));
    drive(0, 1000);
    TEST_ASSERT_EQUAL_INT(ISO_RES_ALREADY_READY, iso_core_submit_finish(&CORE, id, NULL));
    TEST_ASSERT_EQUAL_UINT(0, iso_core_queue_len(&CORE));
}

void test_finish_size_mismatch_reports_received(void) {
    char id[9];
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_upload_begin(up(), "live", 4, SZ, NULL, id));
    put_all(id, 0, 5000, 5000);
    uint64_t rec = 0;
    TEST_ASSERT_EQUAL_INT(ISO_RES_SIZE_MISMATCH, iso_core_submit_finish(&CORE, id, &rec));
    TEST_ASSERT_EQUAL_UINT64(5000, rec);
    TEST_ASSERT_EQUAL_UINT(0, iso_core_queue_len(&CORE));
    TEST_ASSERT_EQUAL_INT(ISO_STATE_UPLOADING, find(id)->state);
}

/* ---- INDEX ------------------------------------------------------------------------------- */

void test_ready_image_without_a_table_is_shown_as_finalizing_until_indexed(void) {
    make_image("000000a1", "imp", SZ, PB_SMALL, ISO_META_READY, SZ);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_core_boot(&CORE));
    TEST_ASSERT_EQUAL_UINT(1, iso_core_index_pending(&CORE)); /* boot が、ready のイメージを集合に入れる */
    TEST_ASSERT_EQUAL_INT(ISO_STATE_READY, find("000000a1")->state);
    TEST_ASSERT_EQUAL_INT(ISO_STATE_FINALIZING, view_of("000000a1")->state); /* 表がない間は finalizing(枠は使わない) */
    TEST_ASSERT_FALSE(view_of("000000a1")->has_sha);                          /* import: SHA-256 なし */
    TEST_ASSERT_FALSE(VIEW_COPY.upload_active);
    TEST_ASSERT_EQUAL_INT(1, drive(0, 1000));
    TEST_ASSERT_EQUAL_INT(ISO_JOB_INDEX, JOBS[0].kind);
    TEST_ASSERT_EQUAL_INT(ISO_STEP_DONE, JOBS[0].result);
    TEST_ASSERT_EQUAL_INT(ISO_STATE_READY, view_of("000000a1")->state);
    TEST_ASSERT_NOT_NULL(view_of("000000a1")->table);
    TEST_ASSERT_TRUE(view_of("000000a1")->table == iso_core_table(&CORE, "000000a1"));
    TEST_ASSERT_EQUAL_UINT32(EPOCH, iso_extent_table_epoch(view_of("000000a1")->table));
}

void test_index_job_part_mismatch_makes_the_image_invalid_and_drops_the_old_table(void) {
    make_image("000000a1", "x1", SZ, PB_SMALL, ISO_META_READY, SZ);
    scan();
    TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_core_submit(&CORE, ISO_JOB_INDEX, "000000a1"));
    drive(0, 1000);
    iso_extent_table_t *old = iso_core_table(&CORE, "000000a1");
    TEST_ASSERT_NOT_NULL(old);
    iso_extent_table_ref(old); /* MSC の READ が持っている参照 */
    TEST_ASSERT_EQUAL_INT(ISO_OK, IFS.remove(&FS, "/iso/000000a1/p002"));
    TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_core_submit(&CORE, ISO_JOB_INDEX, "000000a1"));
    TEST_ASSERT_EQUAL_INT(1, drive(0, 1000));
    TEST_ASSERT_EQUAL_INT(ISO_STEP_DONE, JOBS[0].result);
    TEST_ASSERT_EQUAL_INT(ISO_STATE_INVALID, find("000000a1")->state);
    TEST_ASSERT_EQUAL_UINT8(ISO_INVALID_PART_MISMATCH, find("000000a1")->invalid_reason);
    expect_meta_state("000000a1", ISO_META_INVALID, ISO_INVALID_PART_MISMATCH);
    TEST_ASSERT_NULL(iso_core_table(&CORE, "000000a1"));
    TEST_ASSERT_EQUAL_INT(ISO_STATE_INVALID, view_of("000000a1")->state);
    TEST_ASSERT_EQUAL_UINT8(ISO_INVALID_PART_MISMATCH, view_of("000000a1")->invalid_reason);
    /* 古い表は、カタログの分だけ放す。READ が持つ参照の分は生きている */
    TEST_ASSERT_EQUAL_UINT32(1, iso_extent_table_refs(old));
    TEST_ASSERT_EQUAL_UINT32(3, iso_extent_table_part_count(old));
    iso_extent_table_unref(old);
}

void test_index_job_too_fragmented(void) {
    make_image("000000a1", "x1", SZ, PB_SMALL, ISO_META_READY, SZ);
    scan();
    TEST_ASSERT_TRUE(mock_fs_fail_extents(&FS, "/iso/000000a1/p000", ISO_ERR_TOO_FRAGMENTED));
    TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_core_submit(&CORE, ISO_JOB_INDEX, "000000a1"));
    drive(0, 1000);
    TEST_ASSERT_EQUAL_UINT8(ISO_INVALID_TOO_FRAGMENTED, find("000000a1")->invalid_reason);
    expect_meta_state("000000a1", ISO_META_INVALID, ISO_INVALID_TOO_FRAGMENTED);
}

void test_index_job_io_error_keeps_ready_without_a_table_and_can_be_retried(void) {
    make_image("000000a1", "x1", SZ, PB_SMALL, ISO_META_READY, SZ);
    scan();
    TEST_ASSERT_TRUE(mock_fs_fail_extents(&FS, "/iso/000000a1/p001", ISO_ERR_IO));
    TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_core_submit(&CORE, ISO_JOB_INDEX, "000000a1"));
    TEST_ASSERT_EQUAL_INT(1, drive(0, 1000));
    TEST_ASSERT_EQUAL_INT(ISO_STEP_ERROR, JOBS[0].result);
    TEST_ASSERT_EQUAL_INT(ISO_ERR_IO, JOBS[0].err);
    TEST_ASSERT_EQUAL_INT(ISO_STATE_READY, find("000000a1")->state);
    expect_meta_state("000000a1", ISO_META_READY, 0);
    TEST_ASSERT_NULL(iso_core_table(&CORE, "000000a1"));
    TEST_ASSERT_EQUAL_INT(ISO_STATE_FINALIZING, view_of("000000a1")->state); /* 表がないので、finalizing のまま */
    TEST_ASSERT_EQUAL_UINT(0, iso_core_index_pending(&CORE));               /* 自動では再試行しない */
    TEST_ASSERT_TRUE(mock_fs_fail_extents(&FS, "/iso/000000a1/p001", 0));
    iso_core_mark_unindexed(&CORE);
    TEST_ASSERT_EQUAL_INT(1, drive(0, 1000));
    TEST_ASSERT_EQUAL_INT(ISO_STEP_DONE, JOBS[0].result);
    TEST_ASSERT_NOT_NULL(iso_core_table(&CORE, "000000a1"));
}

void test_index_job_meta_write_failure_for_an_invalid_image_is_an_error_but_the_state_is_invalid(void) {
    make_image("000000a1", "x1", SZ, PB_SMALL, ISO_META_READY, SZ);
    scan();
    TEST_ASSERT_EQUAL_INT(ISO_OK, IFS.remove(&FS, "/iso/000000a1/p001"));
    TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_core_submit(&CORE, ISO_JOB_INDEX, "000000a1"));
    fx_settle();
    mock_fs_error_at(&FS, 1, ISO_ERR_IO);
    TEST_ASSERT_EQUAL_INT(1, drive(0, 1000));
    TEST_ASSERT_EQUAL_INT(ISO_STEP_ERROR, JOBS[0].result);
    TEST_ASSERT_EQUAL_INT(ISO_ERR_IO, JOBS[0].err);
    TEST_ASSERT_EQUAL_INT(ISO_STATE_INVALID, find("000000a1")->state);
    TEST_ASSERT_EQUAL_UINT8(ISO_INVALID_PART_MISMATCH, find("000000a1")->invalid_reason);
    expect_meta_state("000000a1", ISO_META_READY, 0); /* ディスク上は ready のまま。次の起動で、もう一度判定される */
}

/* ---- IDLE_CLOSE ------------------------------------------------------------------------- */

void test_idle_close_job_makes_the_upload_incomplete(void) {
    char id[9];
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_upload_begin(up(), "live", 4, SZ, NULL, id));
    put_all(id, 0, 5000, 5000);
    TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_core_submit(&CORE, ISO_JOB_IDLE_CLOSE, id));
    TEST_ASSERT_EQUAL_INT(1, drive(0, 1000));
    TEST_ASSERT_EQUAL_INT(ISO_STEP_DONE, JOBS[0].result);
    TEST_ASSERT_EQUAL_INT(ISO_STATE_INCOMPLETE, find(id)->state);
    TEST_ASSERT_EQUAL_INT(ISO_STATE_INCOMPLETE, view_of(id)->state);
    TEST_ASSERT_FALSE(VIEW_COPY.upload_active);
    TEST_ASSERT_EQUAL_UINT64(5000, view_of(id)->received);
    TEST_ASSERT_EQUAL_INT(0, FS.open_files);
    TEST_ASSERT_TRUE(up()->sha_valid); /* SHA-256 の状態は保つ */
}

void test_idle_close_job_error(void) {
    char id[9];
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_upload_begin(up(), "live", 4, SZ, NULL, id));
    put_all(id, 0, 5000, 5000);
    TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_core_submit(&CORE, ISO_JOB_IDLE_CLOSE, id));
    fx_settle();
    mock_fs_error_at(&FS, 1, ISO_ERR_IO);
    TEST_ASSERT_EQUAL_INT(1, drive(0, 1000));
    TEST_ASSERT_EQUAL_INT(ISO_STEP_ERROR, JOBS[0].result);
    TEST_ASSERT_EQUAL_INT(ISO_ERR_IO, JOBS[0].err);
    TEST_ASSERT_EQUAL_INT(ISO_STATE_INCOMPLETE, find(id)->state);
    TEST_ASSERT_FALSE(iso_upload_slot_busy(up()));
}

void test_idle_close_job_for_an_image_that_is_not_uploading_is_a_noop(void) {
    make_image("000000a1", "x1", SZ, PB_SMALL, ISO_META_INCOMPLETE, 1000);
    scan();
    TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_core_submit(&CORE, ISO_JOB_IDLE_CLOSE, "000000a1"));
    fx_settle();
    TEST_ASSERT_EQUAL_INT(1, drive(0, 1000));
    TEST_ASSERT_EQUAL_INT(ISO_STEP_DONE, JOBS[0].result);
    TEST_ASSERT_EQUAL_INT(0, FS.nlog);
}

/* ---- DELETE ----------------------------------------------------------------------------- */

void test_delete_job_removes_the_image_and_refreshes_the_free_space(void) {
    make_image("000000a1", "x1", SZ, PB_SMALL, ISO_META_INCOMPLETE, SZ);
    scan();
    mock_fs_set_mount_info(&FS, 64 * GIB, 40 * GIB, 32768);
    TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_core_submit_delete(&CORE, "000000a1"));
    TEST_ASSERT_EQUAL_INT(ISO_STATE_DELETING, view_of("000000a1")->state); /* すぐ deleting */
    TEST_ASSERT_EQUAL_INT(1, drive(0, 1000));
    TEST_ASSERT_EQUAL_INT(ISO_STEP_DONE, JOBS[0].result);
    TEST_ASSERT_NULL(find("000000a1"));
    TEST_ASSERT_NULL(view_of("000000a1"));
    TEST_ASSERT_EQUAL_UINT(0, VIEW_COPY.count);
    TEST_ASSERT_FALSE(mock_fs_exists(&FS, "/iso/000000a1"));
    TEST_ASSERT_EQUAL_UINT64(40 * GIB, VIEW_COPY.free_bytes); /* DELETE の完了で、free_bytes を採り直す */
}

void test_delete_job_without_a_table_does_not_wait(void) {
    make_image("000000a1", "x1", SZ, PB_SMALL, ISO_META_READY, SZ); /* 表がない(INDEX 前) */
    scan();
    TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_core_submit_delete(&CORE, "000000a1"));
    iso_job_t *j = iso_core_pick(&CORE);
    iso_step_result_t r;
    do {
        r = iso_job_step(&CORE, j, 0);
        TEST_ASSERT_FALSE(iso_job_waiting(j));
    } while (r.kind == ISO_STEP_CONTINUE);
    TEST_ASSERT_EQUAL_INT(ISO_STEP_DONE, r.kind);
}

void test_delete_job_with_only_the_catalog_reference_does_not_wait_and_frees_the_table(void) {
    make_image("000000a1", "x1", SZ, PB_SMALL, ISO_META_READY, SZ);
    scan();
    TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_core_submit(&CORE, ISO_JOB_INDEX, "000000a1"));
    drive(0, 1000);
    TEST_ASSERT_EQUAL_UINT32(1, iso_extent_table_refs(iso_core_table(&CORE, "000000a1")));
    int live_with_table = ALLOC_LIVE;
    TEST_ASSERT_TRUE(live_with_table > 0);
    TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_core_submit_delete(&CORE, "000000a1"));
    TEST_ASSERT_NOT_NULL(view_of("000000a1")); /* deleting。表は、ビューには出さない */
    TEST_ASSERT_NULL(view_of("000000a1")->table);
    iso_job_t *j = iso_core_pick(&CORE);
    iso_step_result_t r = iso_job_step(&CORE, j, 0);
    TEST_ASSERT_EQUAL_INT(ISO_STEP_CONTINUE, r.kind);
    TEST_ASSERT_FALSE(iso_job_waiting(j));
    TEST_ASSERT_EQUAL_INT(0, ALLOC_LIVE); /* 参照が 0 になって、表が解放された */
    TEST_ASSERT_NULL(iso_core_table(&CORE, "000000a1"));
    r = run_job(j, 0, 1000, 100, NULL);
    TEST_ASSERT_EQUAL_INT(ISO_STEP_DONE, r.kind);
}

void test_delete_job_waits_for_readers_then_proceeds(void) {
    make_image("000000a1", "x1", SZ, PB_SMALL, ISO_META_READY, SZ);
    scan();
    TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_core_submit(&CORE, ISO_JOB_INDEX, "000000a1"));
    drive(0, 1000);
    iso_extent_table_t *t = iso_core_table(&CORE, "000000a1");
    iso_extent_table_ref(t); /* 進行中の READ */
    TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_core_submit_delete(&CORE, "000000a1"));
    iso_job_t *j = iso_core_pick(&CORE);
    fx_settle();
    for (int i = 0; i < 5; i++) {
        iso_step_result_t r = iso_job_step(&CORE, j, 0);
        TEST_ASSERT_EQUAL_INT(ISO_STEP_CONTINUE, r.kind);
        TEST_ASSERT_TRUE(iso_job_waiting(j));
        TEST_ASSERT_EQUAL_INT(0, FS.nlog); /* 待っている間は、何も消さない */
        NOW_MS += 1000;
    }
    TEST_ASSERT_NOT_NULL(iso_core_table(&CORE, "000000a1")); /* 表は、まだ持っている */
    iso_extent_table_unref(t); /* mmc_scsi_command_done */
    iso_step_result_t r = iso_job_step(&CORE, j, 0);
    TEST_ASSERT_FALSE(iso_job_waiting(j));
    TEST_ASSERT_EQUAL_INT(ISO_STEP_CONTINUE, r.kind);
    r = run_job(j, 0, 1000, 100, NULL);
    TEST_ASSERT_EQUAL_INT(ISO_STEP_DONE, r.kind);
    TEST_ASSERT_NULL(find("000000a1"));
}

void test_delete_ref_wait_gives_up_at_30_seconds(void) {
    make_image("000000a1", "x1", SZ, PB_SMALL, ISO_META_READY, SZ);
    scan();
    TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_core_submit(&CORE, ISO_JOB_INDEX, "000000a1"));
    drive(0, 1000);
    iso_extent_table_t *t = iso_core_table(&CORE, "000000a1");
    iso_extent_table_ref(t);
    TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_core_submit_delete(&CORE, "000000a1"));
    iso_job_t *j = iso_core_pick(&CORE);
    fx_settle();
    NOW_MS = 5000000;
    iso_step_result_t r = iso_job_step(&CORE, j, 0); /* 待ち始め */
    TEST_ASSERT_EQUAL_INT(ISO_STEP_CONTINUE, r.kind);
    NOW_MS = 5000000 + 29999;
    r = iso_job_step(&CORE, j, 0);
    TEST_ASSERT_EQUAL_INT(ISO_STEP_CONTINUE, r.kind);
    TEST_ASSERT_TRUE(iso_job_waiting(j));
    NOW_MS = 5000000 + 30000; /* ちょうど 30 秒 */
    r = iso_job_step(&CORE, j, 0);
    TEST_ASSERT_EQUAL_INT(ISO_STEP_ERROR, r.kind);
    TEST_ASSERT_EQUAL_INT(ISO_ERR_BUSY, r.err);
    /* deleting のまま。何も消していない。表もある */
    TEST_ASSERT_EQUAL_INT(ISO_STATE_DELETING, find("000000a1")->state);
    TEST_ASSERT_EQUAL_INT(0, FS.nlog);
    TEST_ASSERT_NOT_NULL(iso_core_table(&CORE, "000000a1"));
    TEST_ASSERT_NULL(iso_core_pick(&CORE)); /* ジョブは終わった */
    /* 次の機会(再度の DELETE): 参照が消えていれば、続けて完了する */
    iso_extent_table_unref(t);
    TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_core_submit_delete(&CORE, "000000a1"));
    TEST_ASSERT_EQUAL_INT(1, drive(0, 1000));
    TEST_ASSERT_EQUAL_INT(ISO_STEP_DONE, JOBS[0].result);
    TEST_ASSERT_NULL(find("000000a1"));
}

void test_delete_ref_wait_with_a_clock_that_goes_back_counts_zero_elapsed(void) {
    make_image("000000a1", "x1", SZ, PB_SMALL, ISO_META_READY, SZ);
    scan();
    TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_core_submit(&CORE, ISO_JOB_INDEX, "000000a1"));
    drive(0, 1000);
    iso_extent_table_t *t = iso_core_table(&CORE, "000000a1");
    iso_extent_table_ref(t);
    TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_core_submit_delete(&CORE, "000000a1"));
    iso_job_t *j = iso_core_pick(&CORE);
    NOW_MS = 8000000;
    iso_job_step(&CORE, j, 0); /* 待ち始め */
    NOW_MS = 1000000;          /* 時計が巻き戻った: 経過 0、基準を引き直す */
    iso_step_result_t r = iso_job_step(&CORE, j, 0);
    TEST_ASSERT_EQUAL_INT(ISO_STEP_CONTINUE, r.kind);
    NOW_MS = 1000000 + 29999;
    r = iso_job_step(&CORE, j, 0);
    TEST_ASSERT_EQUAL_INT(ISO_STEP_CONTINUE, r.kind);
    NOW_MS = 1000000 + 30000;
    r = iso_job_step(&CORE, j, 0);
    TEST_ASSERT_EQUAL_INT(ISO_STEP_ERROR, r.kind);
    TEST_ASSERT_EQUAL_INT(ISO_ERR_BUSY, r.err);
    iso_extent_table_unref(t);
}

void test_delete_job_io_error_keeps_deleting_and_a_second_delete_continues(void) {
    make_image("000000a1", "x1", SZ, PB_SMALL, ISO_META_INCOMPLETE, SZ);
    scan();
    TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_core_submit_delete(&CORE, "000000a1"));
    fx_settle();
    mock_fs_error_at(&FS, 2, ISO_ERR_IO); /* (1) meta、(2) 最初のパーツの remove が失敗 */
    TEST_ASSERT_EQUAL_INT(1, drive(0, 1000));
    TEST_ASSERT_EQUAL_INT(ISO_STEP_ERROR, JOBS[0].result);
    TEST_ASSERT_EQUAL_INT(ISO_ERR_IO, JOBS[0].err);
    TEST_ASSERT_EQUAL_INT(ISO_STATE_DELETING, find("000000a1")->state);
    TEST_ASSERT_NULL(iso_core_pick(&CORE));
    mock_fs_reset_counter(&FS);
    TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_core_submit_delete(&CORE, "000000a1")); /* deleting でジョブなし: 積み直す */
    TEST_ASSERT_EQUAL_INT(1, drive(0, 1000));
    TEST_ASSERT_EQUAL_INT(ISO_STEP_DONE, JOBS[0].result);
    TEST_ASSERT_NULL(find("000000a1"));
}

/* ---- ビュー ----------------------------------------------------------------------------- */

void test_view_contents(void) {
    make_image("000000a1", "ready-img", SZ, PB_SMALL, ISO_META_READY, SZ);
    make_image("000000a2", "inc", SZ, PB_SMALL, ISO_META_INCOMPLETE, 5000);
    iso_meta_t inv = mk_meta("bad", SZ, PB_SMALL, ISO_META_INVALID, 3);
    inv.invalid_reason = ISO_INVALID_NOT_ISO;
    put_meta("000000a3", &inv);
    put_parts_data("000000a3", PB_SMALL, SZ);
    TEST_ASSERT_EQUAL_INT(ISO_OK, mock_fs_add_dir(&FS, "/iso/000000a4")); /* meta_corrupt */
    mock_fs_set_mount_info(&FS, 32 * GIB, 20 * GIB, 32768);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_core_boot(&CORE));
    TEST_ASSERT_EQUAL_UINT(4, VIEW_COPY.count);
    TEST_ASSERT_EQUAL_INT(ISO_SD_READY, VIEW_COPY.sd_state);
    TEST_ASSERT_TRUE(VIEW_COPY.free_known);
    TEST_ASSERT_EQUAL_UINT64(32 * GIB, VIEW_COPY.total_bytes);
    TEST_ASSERT_EQUAL_UINT64(20 * GIB, VIEW_COPY.free_bytes);
    TEST_ASSERT_EQUAL_UINT32(131072, VIEW_COPY.chunk_bytes);
    TEST_ASSERT_EQUAL_UINT32(0, VIEW_COPY.hidden);
    const iso_view_image_t *a = view_of("000000a1");
    TEST_ASSERT_EQUAL_STRING("ready-img", a->name);
    TEST_ASSERT_EQUAL_UINT8(9, a->name_len);
    TEST_ASSERT_EQUAL_UINT64(SZ, a->size_bytes);
    TEST_ASSERT_EQUAL_UINT64(SZ, a->received);
    TEST_ASSERT_EQUAL_INT(ISO_STATE_FINALIZING, a->state); /* ready で、表がまだない */
    const iso_view_image_t *b = view_of("000000a2");
    TEST_ASSERT_EQUAL_INT(ISO_STATE_INCOMPLETE, b->state);
    TEST_ASSERT_EQUAL_UINT64(5000, b->received);
    TEST_ASSERT_NULL(b->table);
    const iso_view_image_t *c = view_of("000000a3");
    TEST_ASSERT_EQUAL_INT(ISO_STATE_INVALID, c->state);
    TEST_ASSERT_EQUAL_UINT8(ISO_INVALID_NOT_ISO, c->invalid_reason);
    TEST_ASSERT_TRUE(c->meta_valid);
    const iso_view_image_t *d = view_of("000000a4");
    TEST_ASSERT_EQUAL_INT(ISO_STATE_INVALID, d->state);
    TEST_ASSERT_EQUAL_UINT8(ISO_INVALID_META_CORRUPT, d->invalid_reason);
    TEST_ASSERT_FALSE(d->meta_valid);
    TEST_ASSERT_EQUAL_UINT8(0, d->name_len);
    TEST_ASSERT_EQUAL_UINT64(0, d->size_bytes);
}

void test_view_reports_the_sd_state_and_hidden_images(void) {
    for (int i = 0; i < 18; i++) {
        char id[9];
        snprintf(id, sizeof(id), "000002%02d", i);
        char path[32];
        snprintf(path, sizeof(path), "/iso/%s", id);
        if (!mock_fs_exists(&FS, path)) TEST_ASSERT_EQUAL_INT(ISO_OK, mock_fs_add_dir(&FS, path));
    }
    SD_STATE = ISO_SD_UNSUPPORTED_FS;
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_core_boot(&CORE));
    TEST_ASSERT_EQUAL_UINT(16, VIEW_COPY.count);
    TEST_ASSERT_EQUAL_UINT32(2, VIEW_COPY.hidden);
    TEST_ASSERT_EQUAL_INT(ISO_SD_UNSUPPORTED_FS, VIEW_COPY.sd_state);
    SD_STATE = ISO_SD_READY;
    iso_core_publish(&CORE);
    TEST_ASSERT_EQUAL_INT(ISO_SD_READY, VIEW_COPY.sd_state);
    TEST_ASSERT_TRUE(iso_core_view(&CORE)->count == 16);
}

void test_view_without_hooks_and_sd_status_does_not_crash(void) {
    iso_core_cfg_t cfg;
    fx_core_cfg(&cfg, 0, 0);
    cfg.sd = NULL;
    cfg.hooks.view_update = NULL;
    cfg.hooks.is_mounted = NULL;
    iso_core_t c2;
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_core_init(&c2, &cfg));
    iso_upload_set_free(iso_core_upload(&c2), 64 * GIB, 60 * GIB);
    make_image("000000a1", "x1", SZ, PB_SMALL, ISO_META_INCOMPLETE, 1000);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_core_boot(&c2));
    TEST_ASSERT_EQUAL_INT(ISO_SD_NO_CARD, iso_core_view(&c2)->sd_state);
    TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_core_submit_delete(&c2, "000000a1")); /* マウント中の問い合わせがなければ、マウントなし */
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_core_boot(&c2)); /* 後始末 */
}

void test_the_end_of_a_job_publishes_the_view(void) {
    make_image("000000a1", "x1", SZ, PB_SMALL, ISO_META_READY, SZ);
    scan();
    TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_core_submit(&CORE, ISO_JOB_INDEX, "000000a1"));
    iso_job_t *j = iso_core_pick(&CORE);
    iso_step_result_t r;
    int before = VIEW_UPDATES;
    do {
        r = iso_job_step(&CORE, j, 0);
        if (r.kind == ISO_STEP_CONTINUE) TEST_ASSERT_EQUAL_INT(before, VIEW_UPDATES); /* 途中では、状態は変わらない */
    } while (r.kind == ISO_STEP_CONTINUE);
    TEST_ASSERT_EQUAL_INT(before + 1, VIEW_UPDATES);
    TEST_ASSERT_NOT_NULL(view_of("000000a1")->table); /* ビューに、表が出ている */
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_finish_job_end_to_end);
    RUN_TEST(test_view_in_the_middle_of_a_finish_job);
    RUN_TEST(test_finish_job_io_error_becomes_incomplete_and_the_view_follows);
    RUN_TEST(test_finish_job_with_a_wrong_signature_ends_invalid_with_done);
    RUN_TEST(test_finish_of_a_ready_image_is_idempotent);
    RUN_TEST(test_finish_size_mismatch_reports_received);
    RUN_TEST(test_ready_image_without_a_table_is_shown_as_finalizing_until_indexed);
    RUN_TEST(test_index_job_part_mismatch_makes_the_image_invalid_and_drops_the_old_table);
    RUN_TEST(test_index_job_too_fragmented);
    RUN_TEST(test_index_job_io_error_keeps_ready_without_a_table_and_can_be_retried);
    RUN_TEST(test_index_job_meta_write_failure_for_an_invalid_image_is_an_error_but_the_state_is_invalid);
    RUN_TEST(test_idle_close_job_makes_the_upload_incomplete);
    RUN_TEST(test_idle_close_job_error);
    RUN_TEST(test_idle_close_job_for_an_image_that_is_not_uploading_is_a_noop);
    RUN_TEST(test_delete_job_removes_the_image_and_refreshes_the_free_space);
    RUN_TEST(test_delete_job_without_a_table_does_not_wait);
    RUN_TEST(test_delete_job_with_only_the_catalog_reference_does_not_wait_and_frees_the_table);
    RUN_TEST(test_delete_job_waits_for_readers_then_proceeds);
    RUN_TEST(test_delete_ref_wait_gives_up_at_30_seconds);
    RUN_TEST(test_delete_ref_wait_with_a_clock_that_goes_back_counts_zero_elapsed);
    RUN_TEST(test_delete_job_io_error_keeps_deleting_and_a_second_delete_continues);
    RUN_TEST(test_view_contents);
    RUN_TEST(test_view_reports_the_sd_state_and_hidden_images);
    RUN_TEST(test_view_without_hooks_and_sd_status_does_not_crash);
    RUN_TEST(test_the_end_of_a_job_publishes_the_view);
    return UNITY_END();
}
