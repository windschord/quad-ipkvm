/*
 * iso_core_sd_reinit / iso_core_boot(設計 3.4 末尾の ②〜⑦ と、起動時)。呼び出し順の契約: ジョブの全取り消し → ファイルを捨てる →
 * アップロードを incomplete に → remount(⑤。ここではカタログは古いまま)→ カタログの再構築 → INDEX の集合と DELETE の再投入。
 */
#include "core_common.h"

void setUp(void) { fx_core_setup(0, 0); }
void tearDown(void) { fx_core_teardown(); }

#define SZ 100000ull

static iso_upload_t *up(void) { return iso_core_upload(&CORE); }
static iso_catalog_entry_t *find(const char *id) { return iso_catalog_find(&CAT, id); }

static const iso_view_image_t *view_of(const char *id) {
    for (size_t i = 0; i < VIEW_COPY.count; i++) {
        if (strcmp(VIEW_COPY.images[i].id, id) == 0) return &VIEW_COPY.images[i];
    }
    return NULL;
}

/* remount の時点の観測(⑤ の時点で、②③④ が済み、⑥ が済んでいないこと) */
typedef struct {
    int calls;
    int ret;
    size_t qn, index_n;
    bool cur_active;
    bool table_a1;
    int open_files;
    bool slot_active, sha_valid;
    size_t catalog_count;
    iso_state_t live_state;
    int alloc_live;
    int view_updates;
} obs_t;
static obs_t OBS;

static int remount_cb(void *ctx) {
    (void)ctx;
    OBS.calls++;
    OBS.qn = iso_core_queue_len(&CORE);
    OBS.index_n = iso_core_index_pending(&CORE);
    OBS.cur_active = CORE.cur_active;
    OBS.table_a1 = iso_core_table(&CORE, "000000a1") != NULL;
    OBS.open_files = FS.open_files;
    OBS.slot_active = up()->slot_active;
    OBS.sha_valid = up()->sha_valid;
    OBS.catalog_count = CAT.count;
    iso_catalog_entry_t *e = iso_catalog_find(&CAT, "11110001");
    OBS.live_state = e ? e->state : ISO_STATE_NONE;
    OBS.alloc_live = ALLOC_LIVE;
    OBS.view_updates = VIEW_UPDATES;
    return OBS.ret;
}

void test_reinit_cancels_everything_before_remount_and_rebuilds_after(void) {
    make_image("000000a1", "r1", SZ, PB_SMALL, ISO_META_READY, SZ);
    make_image("000000a2", "r2", SZ, PB_SMALL, ISO_META_READY, SZ);
    make_image("000000a3", "i3", SZ, PB_SMALL, ISO_META_INCOMPLETE, 3000);
    make_image("000000a4", "r4", SZ, PB_SMALL, ISO_META_READY, SZ);
    make_image("000000a5", "i5", SZ, PB_SMALL, ISO_META_INCOMPLETE, 2000);
    iso_meta_t dm = mk_meta("d6", SZ, PB_SMALL, ISO_META_DELETING, 9);
    put_meta("000000a6", &dm);
    put_parts_data("000000a6", PB_SMALL, SZ);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_core_boot(&CORE));
    drive(0, 1000); /* DELETE a6、INDEX a1 / a2 / a4 */
    TEST_ASSERT_NULL(find("000000a6"));
    TEST_ASSERT_NOT_NULL(iso_core_table(&CORE, "000000a1"));
    iso_extent_table_t *old_a1 = iso_core_table(&CORE, "000000a1");
    iso_extent_table_ref(old_a1); /* MSC の READ が持っている */

    /* 実行中のアップロード(ファイルを開いている) */
    char id[9];
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_upload_begin(up(), "live", 4, SZ, NULL, id));
    TEST_ASSERT_EQUAL_STRING("11110001", id);
    put_all(id, 0, 5000, 5000);
    TEST_ASSERT_EQUAL_INT(1, FS.open_files);
    /* 待っているジョブと、動いているジョブ、INDEX の集合 */
    TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_core_submit(&CORE, ISO_JOB_IDLE_CLOSE, "000000a3"));
    TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_core_submit_delete(&CORE, "000000a5"));
    TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_core_submit(&CORE, ISO_JOB_INDEX, "000000a2")); /* 再投入 */
    iso_job_t *j = iso_core_pick(&CORE); /* DELETE a5 が動く(まだ何も書いていない) */
    TEST_ASSERT_EQUAL_INT(ISO_JOB_DELETE, j->kind);
    TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_core_submit(&CORE, ISO_JOB_INDEX, "000000a4"));
    TEST_ASSERT_EQUAL_UINT(1, iso_core_queue_len(&CORE));
    TEST_ASSERT_EQUAL_UINT(2, iso_core_index_pending(&CORE));
    mock_fs_set_mount_info(&FS, 64 * GIB, 33 * GIB, 32768);

    memset(&OBS, 0, sizeof(OBS));
    int updates_before = VIEW_UPDATES;
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_core_sd_reinit(&CORE, remount_cb, NULL));
    TEST_ASSERT_EQUAL_INT(1, OBS.calls);
    /* ⑤ の時点: ②③④ が済んでいて、⑥ はまだ */
    TEST_ASSERT_EQUAL_UINT(0, OBS.qn);
    TEST_ASSERT_EQUAL_UINT(0, OBS.index_n);
    TEST_ASSERT_FALSE(OBS.cur_active);
    TEST_ASSERT_FALSE(OBS.table_a1);          /* 表(カタログの分の参照)は捨てた */
    TEST_ASSERT_EQUAL_INT(0, OBS.open_files); /* ファイルハンドルは全部捨てた */
    TEST_ASSERT_FALSE(OBS.slot_active);       /* 枠を空けた */
    TEST_ASSERT_FALSE(OBS.sha_valid);         /* SHA-256 の状態を捨てた */
    TEST_ASSERT_EQUAL_UINT(6, OBS.catalog_count); /* カタログは、古いまま(a1〜a5 と live) */
    TEST_ASSERT_EQUAL_INT(ISO_STATE_INCOMPLETE, OBS.live_state); /* uploading は incomplete */
    TEST_ASSERT_TRUE(OBS.alloc_live > 0);     /* READ が持つ参照の分の表は生きている(遅延解放) */
    TEST_ASSERT_EQUAL_INT(updates_before, OBS.view_updates); /* ビューの更新は、最後 */
    /* ⑥⑦ の後 */
    TEST_ASSERT_EQUAL_UINT(6, CAT.count);     /* ディスクから作り直した(live は uploading のまま = incomplete) */
    TEST_ASSERT_EQUAL_INT(ISO_STATE_INCOMPLETE, find("11110001")->state);
    TEST_ASSERT_EQUAL_UINT64(5000, find("11110001")->received); /* 閉じたときに同期された */
    TEST_ASSERT_EQUAL_INT(ISO_STATE_INCOMPLETE, find("000000a5")->state); /* DELETE のジョブは取り消され、meta は deleting になっていない */
    TEST_ASSERT_EQUAL_UINT(0, iso_core_queue_len(&CORE));
    TEST_ASSERT_EQUAL_UINT(3, iso_core_index_pending(&CORE)); /* a1 a2 a4: ready のイメージごとに INDEX */
    TEST_ASSERT_NULL(iso_core_table(&CORE, "000000a1"));
    TEST_ASSERT_EQUAL_UINT64(33 * GIB, up()->free_bytes);     /* free_bytes のキャッシュを採り直した */
    TEST_ASSERT_TRUE(VIEW_UPDATES > updates_before);
    TEST_ASSERT_EQUAL_UINT(6, VIEW_COPY.count);
    TEST_ASSERT_EQUAL_INT(ISO_STATE_FINALIZING, view_of("000000a1")->state); /* 表がない */
    TEST_ASSERT_FALSE(VIEW_COPY.upload_active);
    /* 古い表は、READ の参照だけが残る。放すと解放される */
    TEST_ASSERT_EQUAL_UINT32(1, iso_extent_table_refs(old_a1));
    TEST_ASSERT_EQUAL_UINT32(3, iso_extent_table_part_count(old_a1));
    iso_extent_table_unref(old_a1);
    /* INDEX が走って、表が作り直される */
    TEST_ASSERT_EQUAL_INT(3, drive(0, 1000));
    TEST_ASSERT_NOT_NULL(iso_core_table(&CORE, "000000a1"));
    TEST_ASSERT_NOT_NULL(iso_core_table(&CORE, "000000a2"));
    TEST_ASSERT_NOT_NULL(iso_core_table(&CORE, "000000a4"));
}

/* 表が満杯(ISO_MAX_IMAGES 個)のときの再初期化は、全部の表を放し、配列の外を読まない */
void test_reinit_with_every_image_indexed_releases_all_sixteen_tables(void) {
    for (int i = 0; i < (int)ISO_MAX_IMAGES; i++) {
        char id[9], name[8];
        snprintf(id, sizeof(id), "000004%02d", i);
        snprintf(name, sizeof(name), "r%d", i);
        make_image(id, name, 34816, PB_SMALL, ISO_META_READY, 34816);
    }
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_core_boot(&CORE));
    TEST_ASSERT_EQUAL_INT(16, drive(0, 1000));
    TEST_ASSERT_EQUAL_UINT(ISO_MAX_IMAGES, CORE.table_n);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_core_sd_reinit(&CORE, NULL, NULL));
    TEST_ASSERT_EQUAL_UINT(0, CORE.table_n);
    TEST_ASSERT_EQUAL_UINT(ISO_MAX_IMAGES, iso_core_index_pending(&CORE)); /* 全部、もう一度 INDEX が必要 */
    TEST_ASSERT_EQUAL_INT(16, drive(0, 1000));
    TEST_ASSERT_EQUAL_UINT(ISO_MAX_IMAGES, CORE.table_n);
}

void test_reinit_resumes_deleting_images(void) {
    iso_meta_t dm = mk_meta("d1", SZ, PB_SMALL, ISO_META_DELETING, 9);
    put_meta("000000a1", &dm);
    put_parts_data("000000a1", PB_SMALL, SZ);
    make_image("000000a2", "r2", SZ, PB_SMALL, ISO_META_READY, SZ);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_core_sd_reinit(&CORE, NULL, NULL));
    TEST_ASSERT_EQUAL_UINT(1, iso_core_queue_len(&CORE));
    TEST_ASSERT_EQUAL_INT(ISO_STATE_DELETING, find("000000a1")->state);
    TEST_ASSERT_EQUAL_UINT(1, iso_core_index_pending(&CORE)); /* deleting の方は、INDEX に入れない */
    TEST_ASSERT_EQUAL_INT(2, drive(0, 1000));
    TEST_ASSERT_EQUAL_INT(ISO_JOB_DELETE, JOBS[0].kind); /* DELETE が先 */
    TEST_ASSERT_EQUAL_INT(ISO_JOB_INDEX, JOBS[1].kind);
    TEST_ASSERT_NULL(find("000000a1"));
}

void test_reinit_with_a_running_finish_job_leaves_the_image_incomplete_and_finish_can_be_retried(void) {
    char id[9];
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_upload_begin(up(), "live", 4, SZ, NULL, id));
    iso_catalog_entry_t *e = find(id);
    iso_meta_t m = e->meta;
    m.part_blocks = PB_SMALL;
    m.part_count = 3;
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_catalog_write_meta(&CAT, e, &m));
    put_all(id, 0, SZ, 20000);
    TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_core_submit_finish(&CORE, id, NULL));
    iso_job_t *j = iso_core_pick(&CORE);
    iso_step_result_t r;
    do {
        r = iso_job_step(&CORE, j, 0);
    } while (r.kind == ISO_STEP_CONTINUE && j->fin.idx.pool == NULL);
    TEST_ASSERT_EQUAL_INT(1, ALLOC_LIVE); /* 作業領域 */
    memset(&OBS, 0, sizeof(OBS));
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_core_sd_reinit(&CORE, remount_cb, NULL));
    TEST_ASSERT_EQUAL_INT(0, OBS.alloc_live);  /* 作業領域は、remount の前に解放された */
    TEST_ASSERT_FALSE(OBS.cur_active);
    TEST_ASSERT_EQUAL_INT(ISO_STATE_INCOMPLETE, OBS.live_state);
    TEST_ASSERT_FALSE(OBS.slot_active);
    TEST_ASSERT_EQUAL_INT(ISO_STATE_INCOMPLETE, find(id)->state);
    TEST_ASSERT_EQUAL_UINT64(SZ, find(id)->received); /* received == size なので、ビューアが finish を送る */
    TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_core_submit_finish(&CORE, id, NULL));
    TEST_ASSERT_EQUAL_INT(1, drive(0, 1000));
    TEST_ASSERT_EQUAL_INT(ISO_STEP_DONE, JOBS[0].result);
    TEST_ASSERT_EQUAL_INT(ISO_STATE_READY, find(id)->state);
}

void test_reinit_with_a_running_delete_job_waiting_for_readers(void) {
    make_image("000000a1", "r1", SZ, PB_SMALL, ISO_META_READY, SZ);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_core_boot(&CORE));
    drive(0, 1000);
    iso_extent_table_t *t = iso_core_table(&CORE, "000000a1");
    iso_extent_table_ref(t);
    TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_core_submit_delete(&CORE, "000000a1"));
    iso_job_t *j = iso_core_pick(&CORE);
    iso_job_step(&CORE, j, 0);
    TEST_ASSERT_TRUE(iso_job_waiting(j));
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_core_sd_reinit(&CORE, NULL, NULL));
    TEST_ASSERT_FALSE(CORE.cur_active);
    /* meta は deleting になっていない(ジョブは何も書かなかった)ので、ready として戻る */
    TEST_ASSERT_EQUAL_INT(ISO_STATE_READY, find("000000a1")->state);
    TEST_ASSERT_EQUAL_UINT(1, iso_core_index_pending(&CORE));
    iso_extent_table_unref(t);
}

void test_remount_failure_leaves_an_empty_catalog_and_no_jobs(void) {
    make_image("000000a1", "r1", SZ, PB_SMALL, ISO_META_READY, SZ);
    make_image("000000a2", "i2", SZ, PB_SMALL, ISO_META_INCOMPLETE, 1000);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_core_boot(&CORE));
    TEST_ASSERT_EQUAL_UINT(2, CAT.count);
    memset(&OBS, 0, sizeof(OBS));
    OBS.ret = ISO_ERR_NO_CARD;
    TEST_ASSERT_EQUAL_INT(ISO_ERR_NO_CARD, iso_core_sd_reinit(&CORE, remount_cb, NULL));
    TEST_ASSERT_EQUAL_UINT(0, CAT.count);
    TEST_ASSERT_EQUAL_UINT(0, iso_core_queue_len(&CORE));
    TEST_ASSERT_EQUAL_UINT(0, iso_core_index_pending(&CORE));
    TEST_ASSERT_EQUAL_UINT(0, VIEW_COPY.count); /* ビューも空 */
    TEST_ASSERT_EQUAL_UINT32(0, CAT.hidden);
    TEST_ASSERT_EQUAL_UINT32(0, VIEW_COPY.hidden);
    TEST_ASSERT_NULL(iso_core_pick(&CORE));
    /* SD が戻ったら、もう一度 */
    OBS.ret = ISO_OK;
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_core_sd_reinit(&CORE, remount_cb, NULL));
    TEST_ASSERT_EQUAL_UINT(2, CAT.count);
    TEST_ASSERT_EQUAL_UINT(1, iso_core_index_pending(&CORE));
}

void test_scan_failure_after_remount_leaves_an_empty_catalog(void) {
    make_image("000000a1", "r1", SZ, PB_SMALL, ISO_META_READY, SZ);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_core_boot(&CORE));
    mock_fs_fail_dir_next(&FS, "/iso");
    TEST_ASSERT_EQUAL_INT(ISO_ERR_IO, iso_core_sd_reinit(&CORE, NULL, NULL));
    TEST_ASSERT_EQUAL_UINT(0, CAT.count);
    TEST_ASSERT_EQUAL_UINT(0, VIEW_COPY.count);
    TEST_ASSERT_EQUAL_UINT32(0, CAT.hidden);
    TEST_ASSERT_EQUAL_UINT32(0, VIEW_COPY.hidden);
    TEST_ASSERT_EQUAL_UINT(0, iso_core_index_pending(&CORE));
    mock_fs_fail_dir_next(&FS, NULL);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_core_boot(&CORE)); /* 戻ったら、起動できる */
    TEST_ASSERT_EQUAL_UINT(1, CAT.count);
}

void test_reinit_is_idempotent(void) {
    make_image("000000a1", "r1", SZ, PB_SMALL, ISO_META_READY, SZ);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_core_sd_reinit(&CORE, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_core_sd_reinit(&CORE, NULL, NULL));
    TEST_ASSERT_EQUAL_UINT(1, iso_core_index_pending(&CORE)); /* 重複しない */
    TEST_ASSERT_EQUAL_UINT(1, CAT.count);
}

/* ---- 起動 ---------------------------------------------------------------------------------- */

void test_boot_turns_uploading_into_incomplete_and_marks_ready_images(void) {
    make_image("000000a1", "u1", SZ, PB_SMALL, ISO_META_UPLOADING, 7000);
    make_image("000000a2", "r2", SZ, PB_SMALL, ISO_META_READY, SZ);
    make_image("000000a3", "n3", SZ, PB_SMALL, ISO_META_INVALID, SZ);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_core_boot(&CORE));
    TEST_ASSERT_EQUAL_INT(ISO_STATE_INCOMPLETE, find("000000a1")->state);
    TEST_ASSERT_EQUAL_UINT64(7000, find("000000a1")->received);
    TEST_ASSERT_FALSE(iso_upload_slot_busy(up())); /* 起動時は枠が空いている */
    TEST_ASSERT_EQUAL_UINT(1, iso_core_index_pending(&CORE));
    TEST_ASSERT_EQUAL_UINT(0, iso_core_queue_len(&CORE));
    TEST_ASSERT_EQUAL_UINT64(60 * GIB, up()->free_bytes); /* mount_info のキャッシュ */
}

void test_boot_queues_at_most_8_resumed_deletes(void) {
    for (int i = 0; i < 10; i++) {
        char id[9];
        snprintf(id, sizeof(id), "000003%02d", i);
        iso_meta_t dm = mk_meta("d", SZ, PB_SMALL, ISO_META_DELETING, 4);
        dm.name[0] = (char)('a' + i);
        put_meta(id, &dm);
    }
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_core_boot(&CORE));
    TEST_ASSERT_EQUAL_UINT(8, iso_core_queue_len(&CORE)); /* あふれた分は、deleting のまま(再度の DELETE か次の起動で続ける) */
    TEST_ASSERT_EQUAL_UINT(10, CAT.count);
    TEST_ASSERT_EQUAL_INT(8, drive(0, 1000));
    TEST_ASSERT_EQUAL_UINT(2, CAT.count);
    for (size_t i = 0; i < CAT.count; i++) {
        TEST_ASSERT_EQUAL_INT(ISO_STATE_DELETING, CAT.images[i].state);
        char id[9];
        memcpy(id, CAT.images[i].id, 9);
        TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_core_submit_delete(&CORE, id));
    }
    TEST_ASSERT_EQUAL_INT(2, drive(0, 1000));
    TEST_ASSERT_EQUAL_UINT(0, CAT.count);
}

void test_boot_discards_previous_runtime_state(void) {
    make_image("000000a1", "r1", SZ, PB_SMALL, ISO_META_READY, SZ);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_core_boot(&CORE));
    drive(0, 1000);
    TEST_ASSERT_NOT_NULL(iso_core_table(&CORE, "000000a1"));
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_core_boot(&CORE));
    TEST_ASSERT_NULL(iso_core_table(&CORE, "000000a1"));
    TEST_ASSERT_EQUAL_UINT(1, iso_core_index_pending(&CORE));
}

void test_free_cache_refresh_failure_does_not_fail_the_boot(void) {
    make_image("000000a1", "r1", SZ, PB_SMALL, ISO_META_INCOMPLETE, 1000);
    mock_fs_fail_mount_info(&FS, ISO_ERR_IO);
    iso_upload_set_free(up(), 8 * GIB, 7 * GIB);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_core_boot(&CORE));
    TEST_ASSERT_EQUAL_UINT64(7 * GIB, up()->free_bytes); /* 古いキャッシュのまま */
    mock_fs_fail_mount_info(&FS, 0);
}

void test_boot_scan_error_is_returned(void) {
    mock_fs_fail_dir_next(&FS, "/iso");
    TEST_ASSERT_EQUAL_INT(ISO_ERR_IO, iso_core_boot(&CORE));
    TEST_ASSERT_EQUAL_UINT(0, CAT.count);
    mock_fs_fail_dir_next(&FS, NULL);
}


void test_hidden_images_are_forgotten_when_the_remount_or_the_scan_fails(void) {
    for (int i = 0; i < 18; i++) {
        char path[32];
        snprintf(path, sizeof(path), "/iso/000005%02d", i);
        TEST_ASSERT_EQUAL_INT(ISO_OK, mock_fs_add_dir(&FS, path));
    }
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_core_boot(&CORE));
    TEST_ASSERT_EQUAL_UINT32(2, CAT.hidden);
    memset(&OBS, 0, sizeof(OBS));
    OBS.ret = ISO_ERR_NO_CARD;
    TEST_ASSERT_EQUAL_INT(ISO_ERR_NO_CARD, iso_core_sd_reinit(&CORE, remount_cb, NULL));
    TEST_ASSERT_EQUAL_UINT32(0, CAT.hidden);
    TEST_ASSERT_EQUAL_UINT32(0, VIEW_COPY.hidden);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_core_boot(&CORE));
    TEST_ASSERT_EQUAL_UINT32(2, CAT.hidden);
    mock_fs_fail_dir_next_after(&FS, "/iso", 19); /* import と 18 個のディレクトリを読んだ後で失敗(隠れた 2 個は数えた後) */
    TEST_ASSERT_EQUAL_INT(ISO_ERR_IO, iso_core_boot(&CORE));
    TEST_ASSERT_EQUAL_UINT32(0, CAT.hidden);
    TEST_ASSERT_EQUAL_UINT32(0, VIEW_COPY.hidden);
    mock_fs_fail_dir_next(&FS, NULL);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_reinit_cancels_everything_before_remount_and_rebuilds_after);
    RUN_TEST(test_reinit_with_every_image_indexed_releases_all_sixteen_tables);
    RUN_TEST(test_reinit_resumes_deleting_images);
    RUN_TEST(test_reinit_with_a_running_finish_job_leaves_the_image_incomplete_and_finish_can_be_retried);
    RUN_TEST(test_reinit_with_a_running_delete_job_waiting_for_readers);
    RUN_TEST(test_remount_failure_leaves_an_empty_catalog_and_no_jobs);
    RUN_TEST(test_scan_failure_after_remount_leaves_an_empty_catalog);
    RUN_TEST(test_reinit_is_idempotent);
    RUN_TEST(test_boot_turns_uploading_into_incomplete_and_marks_ready_images);
    RUN_TEST(test_boot_queues_at_most_8_resumed_deletes);
    RUN_TEST(test_boot_discards_previous_runtime_state);
    RUN_TEST(test_free_cache_refresh_failure_does_not_fail_the_boot);
    RUN_TEST(test_boot_scan_error_is_returned);
    RUN_TEST(test_hidden_images_are_forgotten_when_the_remount_or_the_scan_fails);
    return UNITY_END();
}
