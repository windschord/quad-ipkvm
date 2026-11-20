/*
 * iso_core のジョブの契約(設計 4.2、INTERFACES 7.5): キュー(長さ 8 の FIFO)と INDEX の集合、優先順位(DELETE、FINISH、IDLE_CLOSE、INDEX)、
 * 同じイメージのジョブの合流、あふれ(ISO_ERR_BUSY。状態を変えない)、DELETE の割り込み(中止の印、枠の解放)。期待値は手書き。
 */
#include "core_common.h"

void setUp(void) { fx_core_setup(0, 0); }
void tearDown(void) { fx_core_teardown(); }

#define SZ 100000ull

static iso_upload_t *up(void) { return iso_core_upload(&CORE); }

static void scan(void) { TEST_ASSERT_EQUAL_INT(ISO_OK, iso_catalog_scan(&CAT)); }

static iso_catalog_entry_t *find(const char *id) { return iso_catalog_find(&CAT, id); }

/* ---- 優先順位 -------------------------------------------------------------------------- */

void test_pick_with_nothing_queued_returns_null(void) {
    TEST_ASSERT_NULL(iso_core_pick(&CORE));
    TEST_ASSERT_EQUAL_UINT(0, iso_core_queue_len(&CORE));
    TEST_ASSERT_EQUAL_UINT(0, iso_core_index_pending(&CORE));
}

void test_priority_is_delete_finish_idle_close_index(void) {
    make_image("000000a1", "x1", SZ, PB_SMALL, ISO_META_READY, SZ);
    make_image("000000a2", "x2", SZ, PB_SMALL, ISO_META_INCOMPLETE, SZ); /* 送り切った */
    make_image("000000a3", "x3", SZ, PB_SMALL, ISO_META_INCOMPLETE, 5000);
    make_image("000000a4", "x4", SZ, PB_SMALL, ISO_META_INCOMPLETE, 3000);
    make_image("000000a5", "x5", SZ, PB_SMALL, ISO_META_INCOMPLETE, 1000);
    scan();
    /* 入れる順は、優先順位と逆 */
    TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_core_submit(&CORE, ISO_JOB_INDEX, "000000a1"));
    TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_core_submit(&CORE, ISO_JOB_IDLE_CLOSE, "000000a4"));
    TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_core_submit(&CORE, ISO_JOB_FINISH, "000000a2"));
    TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_core_submit(&CORE, ISO_JOB_DELETE, "000000a3"));
    TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_core_submit(&CORE, ISO_JOB_DELETE, "000000a5"));
    TEST_ASSERT_EQUAL_UINT(4, iso_core_queue_len(&CORE)); /* INDEX は、キューに積まない */
    TEST_ASSERT_EQUAL_UINT(1, iso_core_index_pending(&CORE));
    TEST_ASSERT_EQUAL_INT(5, drive(0, 1000));
    TEST_ASSERT_EQUAL_INT(ISO_JOB_DELETE, JOBS[0].kind);
    TEST_ASSERT_EQUAL_STRING("000000a3", JOBS[0].id); /* 同じ種類は、入れた順 */
    TEST_ASSERT_EQUAL_INT(ISO_JOB_DELETE, JOBS[1].kind);
    TEST_ASSERT_EQUAL_STRING("000000a5", JOBS[1].id);
    TEST_ASSERT_EQUAL_INT(ISO_JOB_FINISH, JOBS[2].kind);
    TEST_ASSERT_EQUAL_STRING("000000a2", JOBS[2].id);
    TEST_ASSERT_EQUAL_INT(ISO_JOB_IDLE_CLOSE, JOBS[3].kind);
    TEST_ASSERT_EQUAL_STRING("000000a4", JOBS[3].id);
    TEST_ASSERT_EQUAL_INT(ISO_JOB_INDEX, JOBS[4].kind);
    TEST_ASSERT_EQUAL_STRING("000000a1", JOBS[4].id);
    for (int i = 0; i < 5; i++) TEST_ASSERT_EQUAL_INT(ISO_STEP_DONE, JOBS[i].result);
    TEST_ASSERT_NULL(find("000000a3"));
    TEST_ASSERT_NULL(find("000000a5"));
    TEST_ASSERT_EQUAL_INT(ISO_STATE_READY, find("000000a2")->state);
    TEST_ASSERT_NOT_NULL(iso_core_table(&CORE, "000000a2"));
    TEST_ASSERT_NOT_NULL(iso_core_table(&CORE, "000000a1"));
}

void test_a_job_submitted_later_with_higher_priority_goes_first(void) {
    make_image("000000a1", "x1", SZ, PB_SMALL, ISO_META_INCOMPLETE, 1000);
    make_image("000000a2", "x2", SZ, PB_SMALL, ISO_META_INCOMPLETE, 2000);
    scan();
    TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_core_submit(&CORE, ISO_JOB_IDLE_CLOSE, "000000a1"));
    TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_core_submit(&CORE, ISO_JOB_DELETE, "000000a2")); /* 後から入れても、先に動く */
    iso_job_t *j = iso_core_pick(&CORE);
    TEST_ASSERT_NOT_NULL(j);
    TEST_ASSERT_EQUAL_INT(ISO_JOB_DELETE, j->kind);
    TEST_ASSERT_EQUAL_STRING("000000a2", j->id);
}

void test_pick_returns_the_running_job_until_it_ends(void) {
    make_image("000000a1", "x1", SZ, PB_SMALL, ISO_META_INCOMPLETE, 1000);
    make_image("000000a2", "x2", SZ, PB_SMALL, ISO_META_INCOMPLETE, 2000);
    scan();
    TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_core_submit(&CORE, ISO_JOB_DELETE, "000000a1"));
    TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_core_submit(&CORE, ISO_JOB_DELETE, "000000a2"));
    iso_job_t *j1 = iso_core_pick(&CORE);
    iso_job_t *j2 = iso_core_pick(&CORE);
    TEST_ASSERT_TRUE(j1 == j2);
    TEST_ASSERT_EQUAL_STRING("000000a1", j2->id);
    TEST_ASSERT_EQUAL_UINT(1, iso_core_queue_len(&CORE)); /* 動いているものは、キューにない */
    iso_step_result_t r = run_job(j1, 0, 1000, 100, NULL);
    TEST_ASSERT_EQUAL_INT(ISO_STEP_DONE, r.kind);
    iso_job_t *j3 = iso_core_pick(&CORE);
    TEST_ASSERT_NOT_NULL(j3);
    TEST_ASSERT_EQUAL_STRING("000000a2", j3->id);
    run_job(j3, 0, 1000, 100, NULL);
    TEST_ASSERT_NULL(iso_core_pick(&CORE));
}

void test_index_jobs_run_one_at_a_time_in_the_order_they_were_added(void) {
    make_image("000000a1", "x1", SZ, PB_SMALL, ISO_META_READY, SZ);
    make_image("000000a2", "x2", SZ, PB_SMALL, ISO_META_READY, SZ);
    make_image("000000a3", "x3", SZ, PB_SMALL, ISO_META_READY, SZ);
    scan();
    TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_core_submit(&CORE, ISO_JOB_INDEX, "000000a2"));
    TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_core_submit(&CORE, ISO_JOB_INDEX, "000000a3"));
    TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_core_submit(&CORE, ISO_JOB_INDEX, "000000a1"));
    TEST_ASSERT_EQUAL_UINT(3, iso_core_index_pending(&CORE));
    TEST_ASSERT_EQUAL_UINT(0, iso_core_queue_len(&CORE));
    TEST_ASSERT_EQUAL_INT(3, drive(0, 1000));
    TEST_ASSERT_EQUAL_STRING("000000a2", JOBS[0].id);
    TEST_ASSERT_EQUAL_STRING("000000a3", JOBS[1].id);
    TEST_ASSERT_EQUAL_STRING("000000a1", JOBS[2].id);
    TEST_ASSERT_EQUAL_UINT(0, iso_core_index_pending(&CORE));
}

void test_index_jobs_do_not_take_queue_slots(void) {
    for (int i = 0; i < 10; i++) {
        char id[9], name[8];
        snprintf(id, sizeof(id), "000001%02d", i);
        snprintf(name, sizeof(name), "q%d", i);
        make_image(id, name, SZ, PB_SMALL, ISO_META_INCOMPLETE, 1000);
    }
    make_image("000000c1", "r1", SZ, PB_SMALL, ISO_META_READY, SZ);
    scan();
    for (int i = 0; i < 8; i++) {
        char id[9];
        snprintf(id, sizeof(id), "000001%02d", i);
        TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_core_submit(&CORE, ISO_JOB_IDLE_CLOSE, id));
    }
    TEST_ASSERT_EQUAL_UINT(8, iso_core_queue_len(&CORE));
    TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_core_submit(&CORE, ISO_JOB_INDEX, "000000c1")); /* キューが満杯でも、集合には入る */
    TEST_ASSERT_EQUAL_UINT(1, iso_core_index_pending(&CORE));
}

/* ---- あふれ ---------------------------------------------------------------------------- */

static void make_ten_incomplete(void) {
    for (int i = 0; i < 10; i++) {
        char id[9], name[8];
        snprintf(id, sizeof(id), "000001%02d", i);
        snprintf(name, sizeof(name), "q%d", i);
        make_image(id, name, SZ, PB_SMALL, ISO_META_INCOMPLETE, 1000);
    }
}

void test_queue_holds_8_jobs_and_the_9th_is_busy(void) {
    make_ten_incomplete();
    scan();
    for (int i = 0; i < 8; i++) {
        char id[9];
        snprintf(id, sizeof(id), "000001%02d", i);
        TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_core_submit(&CORE, ISO_JOB_IDLE_CLOSE, id));
        TEST_ASSERT_EQUAL_UINT((size_t)i + 1, iso_core_queue_len(&CORE));
    }
    TEST_ASSERT_EQUAL_INT(ISO_ERR_BUSY, iso_core_submit(&CORE, ISO_JOB_IDLE_CLOSE, "00000108"));
    TEST_ASSERT_EQUAL_UINT(8, iso_core_queue_len(&CORE));
    /* 1 つ動かすと、空く */
    iso_job_t *j = iso_core_pick(&CORE);
    TEST_ASSERT_NOT_NULL(j);
    TEST_ASSERT_EQUAL_STRING("00000100", j->id); /* 同じ種類は FIFO */
    TEST_ASSERT_EQUAL_UINT(7, iso_core_queue_len(&CORE));
    TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_core_submit(&CORE, ISO_JOB_IDLE_CLOSE, "00000108"));
    TEST_ASSERT_EQUAL_UINT(8, iso_core_queue_len(&CORE));
}

void test_overflow_of_delete_changes_nothing(void) {
    make_ten_incomplete();
    scan();
    for (int i = 0; i < 8; i++) {
        char id[9];
        snprintf(id, sizeof(id), "000001%02d", i);
        TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_core_submit(&CORE, ISO_JOB_IDLE_CLOSE, id));
    }
    TEST_ASSERT_EQUAL_INT(ISO_ERR_BUSY, iso_core_submit(&CORE, ISO_JOB_DELETE, "00000108"));
    TEST_ASSERT_EQUAL_INT(ISO_STATE_INCOMPLETE, find("00000108")->state); /* deleting にならない */
    TEST_ASSERT_EQUAL_UINT(8, iso_core_queue_len(&CORE));
    /* 待っているジョブがあるイメージの DELETE は、それを置き換えるので、満杯でも受理される */
    TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_core_submit(&CORE, ISO_JOB_DELETE, "00000103"));
    TEST_ASSERT_EQUAL_UINT(8, iso_core_queue_len(&CORE));
    TEST_ASSERT_EQUAL_INT(ISO_STATE_DELETING, find("00000103")->state);
}

void test_overflow_of_finish_rolls_back_an_incomplete_image(void) {
    make_ten_incomplete();
    make_image("000000d1", "full", SZ, PB_SMALL, ISO_META_INCOMPLETE, SZ);
    scan();
    for (int i = 0; i < 8; i++) {
        char id[9];
        snprintf(id, sizeof(id), "000001%02d", i);
        TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_core_submit(&CORE, ISO_JOB_IDLE_CLOSE, id));
    }
    TEST_ASSERT_EQUAL_INT(ISO_ERR_BUSY, iso_core_submit_finish(&CORE, "000000d1", NULL));
    TEST_ASSERT_EQUAL_INT(ISO_STATE_INCOMPLETE, find("000000d1")->state);
    TEST_ASSERT_FALSE(iso_upload_slot_busy(up())); /* 枠を取らない */
    TEST_ASSERT_EQUAL_UINT(8, iso_core_queue_len(&CORE));
    /* 空けば受理 */
    TEST_ASSERT_NOT_NULL(iso_core_pick(&CORE));
    TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_core_submit_finish(&CORE, "000000d1", NULL));
    TEST_ASSERT_EQUAL_INT(ISO_STATE_FINALIZING, find("000000d1")->state);
    TEST_ASSERT_TRUE(iso_upload_slot_busy(up()));
}

void test_overflow_of_finish_rolls_back_an_uploading_image(void) {
    make_ten_incomplete();
    scan();
    char id[9];
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_upload_begin(up(), "live", 4, SZ, NULL, id));
    iso_catalog_entry_t *e = find(id);
    iso_meta_t m = e->meta;
    m.part_blocks = PB_SMALL;
    m.part_count = 3;
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_catalog_write_meta(&CAT, e, &m));
    put_all(id, 0, SZ, 20000);
    for (int i = 0; i < 8; i++) {
        char qid[9];
        snprintf(qid, sizeof(qid), "000001%02d", i);
        TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_core_submit(&CORE, ISO_JOB_IDLE_CLOSE, qid));
    }
    TEST_ASSERT_EQUAL_INT(ISO_ERR_BUSY, iso_core_submit_finish(&CORE, id, NULL));
    TEST_ASSERT_EQUAL_INT(ISO_STATE_UPLOADING, e->state); /* 元の uploading に戻る */
    TEST_ASSERT_TRUE(iso_upload_slot_busy(up()));
    TEST_ASSERT_FALSE(up()->slot_finalizing);
    TEST_ASSERT_EQUAL_INT(1, FS.open_files); /* ハンドルは、そのまま */
    /* アイドルで incomplete にもできる(枠が壊れていない) */
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_upload_idle_close(up(), id));
}

void test_finish_replaces_a_waiting_idle_close_even_when_the_queue_is_full(void) {
    make_ten_incomplete();
    make_image("000000d1", "full", SZ, PB_SMALL, ISO_META_INCOMPLETE, SZ);
    scan();
    TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_core_submit(&CORE, ISO_JOB_IDLE_CLOSE, "000000d1"));
    for (int i = 0; i < 7; i++) {
        char id[9];
        snprintf(id, sizeof(id), "000001%02d", i);
        TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_core_submit(&CORE, ISO_JOB_IDLE_CLOSE, id));
    }
    TEST_ASSERT_EQUAL_UINT(8, iso_core_queue_len(&CORE));
    TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_core_submit_finish(&CORE, "000000d1", NULL));
    TEST_ASSERT_EQUAL_UINT(8, iso_core_queue_len(&CORE));
    TEST_ASSERT_EQUAL_INT(ISO_STATE_FINALIZING, find("000000d1")->state);
}

/* ---- 合流 ---------------------------------------------------------------------------- */

void test_duplicate_submissions_join_the_existing_job(void) {
    make_image("000000a1", "x1", SZ, PB_SMALL, ISO_META_INCOMPLETE, SZ);
    make_image("000000a2", "x2", SZ, PB_SMALL, ISO_META_INCOMPLETE, 1000);
    make_image("000000a3", "x3", SZ, PB_SMALL, ISO_META_READY, SZ);
    scan();
    TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_core_submit(&CORE, ISO_JOB_IDLE_CLOSE, "000000a2"));
    TEST_ASSERT_EQUAL_INT(ISO_RES_JOINED, iso_core_submit(&CORE, ISO_JOB_IDLE_CLOSE, "000000a2"));
    TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_core_submit_finish(&CORE, "000000a1", NULL));
    TEST_ASSERT_EQUAL_INT(ISO_RES_JOINED, iso_core_submit_finish(&CORE, "000000a1", NULL)); /* すでに finalizing: 冪等 */
    TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_core_submit(&CORE, ISO_JOB_INDEX, "000000a3"));
    TEST_ASSERT_EQUAL_INT(ISO_RES_JOINED, iso_core_submit(&CORE, ISO_JOB_INDEX, "000000a3"));
    TEST_ASSERT_EQUAL_UINT(2, iso_core_queue_len(&CORE));
    TEST_ASSERT_EQUAL_UINT(1, iso_core_index_pending(&CORE));
    /* DELETE の重複(a2 の IDLE_CLOSE は置き換わる) */
    TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_core_submit_delete(&CORE, "000000a2"));
    TEST_ASSERT_EQUAL_INT(ISO_RES_JOINED, iso_core_submit_delete(&CORE, "000000a2"));
    TEST_ASSERT_EQUAL_UINT(2, iso_core_queue_len(&CORE)); /* FINISH a1、DELETE a2 */
}

void test_running_job_absorbs_duplicates(void) {
    make_image("000000a1", "x1", SZ, PB_SMALL, ISO_META_INCOMPLETE, SZ);
    scan();
    TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_core_submit_finish(&CORE, "000000a1", NULL));
    iso_job_t *j = iso_core_pick(&CORE);
    TEST_ASSERT_NOT_NULL(j);
    iso_step_result_t r = iso_job_step(&CORE, j, 0);
    TEST_ASSERT_EQUAL_INT(ISO_STEP_CONTINUE, r.kind);
    TEST_ASSERT_EQUAL_INT(ISO_RES_JOINED, iso_core_submit_finish(&CORE, "000000a1", NULL));
    TEST_ASSERT_EQUAL_INT(ISO_RES_JOINED, iso_core_submit(&CORE, ISO_JOB_IDLE_CLOSE, "000000a1")); /* 動いているジョブに合流 */
    TEST_ASSERT_EQUAL_UINT(0, iso_core_queue_len(&CORE));
    run_job(j, 0, 1000, 1000, NULL);
}

void test_finish_replaces_a_waiting_idle_close_of_the_same_image(void) {
    char id[9];
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_upload_begin(up(), "live", 4, 34816, NULL, id));
    put_all(id, 0, 34816, 20000);
    TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_core_submit(&CORE, ISO_JOB_IDLE_CLOSE, id));
    TEST_ASSERT_EQUAL_UINT(1, iso_core_queue_len(&CORE));
    TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_core_submit_finish(&CORE, id, NULL));
    TEST_ASSERT_EQUAL_UINT(1, iso_core_queue_len(&CORE)); /* IDLE_CLOSE は取り除かれ、FINISH だけ */
    iso_job_t *j = iso_core_pick(&CORE);
    TEST_ASSERT_EQUAL_INT(ISO_JOB_FINISH, j->kind);
    run_job(j, 0, 1000, 1000, NULL);
    TEST_ASSERT_EQUAL_INT(ISO_STATE_READY, find(id)->state);
}

void test_idle_close_joins_a_waiting_finish_of_the_same_image(void) {
    char id[9];
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_upload_begin(up(), "live", 4, 34816, NULL, id));
    put_all(id, 0, 34816, 20000);
    TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_core_submit_finish(&CORE, id, NULL));
    TEST_ASSERT_EQUAL_INT(ISO_RES_JOINED, iso_core_submit(&CORE, ISO_JOB_IDLE_CLOSE, id));
    TEST_ASSERT_EQUAL_UINT(1, iso_core_queue_len(&CORE));
}

/* ---- DELETE の割り込み --------------------------------------------------------------------- */

void test_delete_aborts_the_running_finish_job_of_the_same_image(void) {
    make_image("000000a1", "x1", SZ, PB_SMALL, ISO_META_INCOMPLETE, SZ);
    scan();
    TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_core_submit_finish(&CORE, "000000a1", NULL));
    TEST_ASSERT_TRUE(iso_upload_slot_busy(up()));
    iso_job_t *j = iso_core_pick(&CORE);
    iso_step_result_t r;
    do {
        r = iso_job_step(&CORE, j, 0);
    } while (r.kind == ISO_STEP_CONTINUE && j->fin.idx.pool == NULL); /* エクステントの作成の途中まで */
    TEST_ASSERT_EQUAL_INT(ISO_STEP_CONTINUE, r.kind);
    TEST_ASSERT_EQUAL_INT(1, ALLOC_LIVE);
    TEST_ASSERT_FALSE(j->aborted);
    TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_core_submit_delete(&CORE, "000000a1"));
    TEST_ASSERT_TRUE(j->aborted);
    TEST_ASSERT_FALSE(iso_upload_slot_busy(up())); /* 枠は、すぐ空く */
    TEST_ASSERT_EQUAL_INT(ISO_STATE_DELETING, find("000000a1")->state);
    /* 次のステップの先頭で見る: 何もせずに終わり、作業領域を放す */
    fx_settle();
    r = iso_job_step(&CORE, j, 0);
    TEST_ASSERT_EQUAL_INT(ISO_STEP_DONE, r.kind);
    TEST_ASSERT_EQUAL_INT(0, ALLOC_LIVE);
    TEST_ASSERT_EQUAL_INT(0, FS.nlog);
    TEST_ASSERT_NULL(iso_core_table(&CORE, "000000a1"));
    /* DELETE のジョブが続く */
    iso_job_t *d = iso_core_pick(&CORE);
    TEST_ASSERT_NOT_NULL(d);
    TEST_ASSERT_EQUAL_INT(ISO_JOB_DELETE, d->kind);
    r = run_job(d, 0, 1000, 100, NULL);
    TEST_ASSERT_EQUAL_INT(ISO_STEP_DONE, r.kind);
    TEST_ASSERT_NULL(find("000000a1"));
    TEST_ASSERT_FALSE(mock_fs_exists(&FS, "/iso/000000a1"));
}

void test_delete_of_another_image_does_not_abort_the_running_job(void) {
    make_image("000000a1", "x1", SZ, PB_SMALL, ISO_META_INCOMPLETE, SZ);
    make_image("000000a2", "x2", SZ, PB_SMALL, ISO_META_INCOMPLETE, 1000);
    scan();
    TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_core_submit_finish(&CORE, "000000a1", NULL));
    iso_job_t *j = iso_core_pick(&CORE);
    iso_job_step(&CORE, j, 0);
    TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_core_submit_delete(&CORE, "000000a2"));
    TEST_ASSERT_FALSE(j->aborted);
    TEST_ASSERT_TRUE(iso_upload_slot_busy(up())); /* a1 の枠は、そのまま */
    iso_step_result_t r = run_job(j, 0, 1000, 1000, NULL);
    TEST_ASSERT_EQUAL_INT(ISO_STEP_DONE, r.kind);
    TEST_ASSERT_EQUAL_INT(ISO_STATE_READY, find("000000a1")->state);
}

void test_delete_cancels_the_waiting_jobs_of_the_same_image(void) {
    make_image("000000a1", "x1", SZ, PB_SMALL, ISO_META_INCOMPLETE, SZ);
    make_image("000000a2", "x2", SZ, PB_SMALL, ISO_META_READY, SZ);
    scan();
    TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_core_submit_finish(&CORE, "000000a1", NULL));
    TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_core_submit(&CORE, ISO_JOB_INDEX, "000000a2"));
    TEST_ASSERT_EQUAL_UINT(1, iso_core_queue_len(&CORE));
    TEST_ASSERT_EQUAL_UINT(1, iso_core_index_pending(&CORE));
    TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_core_submit_delete(&CORE, "000000a1"));
    TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_core_submit_delete(&CORE, "000000a2"));
    TEST_ASSERT_EQUAL_UINT(2, iso_core_queue_len(&CORE)); /* FINISH は消え、DELETE が 2 つ */
    TEST_ASSERT_EQUAL_UINT(0, iso_core_index_pending(&CORE));
    TEST_ASSERT_FALSE(iso_upload_slot_busy(up()));
    TEST_ASSERT_EQUAL_INT(2, drive(0, 1000));
    TEST_ASSERT_EQUAL_INT(ISO_JOB_DELETE, JOBS[0].kind);
    TEST_ASSERT_EQUAL_INT(ISO_JOB_DELETE, JOBS[1].kind);
    TEST_ASSERT_EQUAL_UINT(0, CAT.count);
}

void test_delete_aborts_the_running_index_job(void) {
    make_image("000000a1", "x1", SZ, PB_SMALL, ISO_META_READY, SZ);
    scan();
    TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_core_submit(&CORE, ISO_JOB_INDEX, "000000a1"));
    iso_job_t *j = iso_core_pick(&CORE);
    iso_job_step(&CORE, j, 0);
    TEST_ASSERT_EQUAL_INT(1, ALLOC_LIVE);
    TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_core_submit_delete(&CORE, "000000a1"));
    TEST_ASSERT_TRUE(j->aborted);
    iso_step_result_t r = iso_job_step(&CORE, j, 0);
    TEST_ASSERT_EQUAL_INT(ISO_STEP_DONE, r.kind);
    TEST_ASSERT_EQUAL_INT(0, ALLOC_LIVE);
    TEST_ASSERT_NULL(iso_core_table(&CORE, "000000a1")); /* 表は作られない */
}

void test_delete_of_an_uploading_image_aborts_the_upload(void) {
    char id[9];
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_upload_begin(up(), "live", 4, SZ, NULL, id));
    put_all(id, 0, 5000, 5000);
    TEST_ASSERT_EQUAL_INT(1, FS.open_files);
    TEST_ASSERT_TRUE(up()->sha_valid);
    TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_core_submit_delete(&CORE, id));
    TEST_ASSERT_EQUAL_INT(0, FS.open_files); /* ファイルを閉じた */
    TEST_ASSERT_FALSE(up()->sha_valid);      /* SHA-256 の状態を捨てた */
    TEST_ASSERT_FALSE(iso_upload_slot_busy(up()));
    TEST_ASSERT_EQUAL_INT(ISO_STATE_DELETING, find(id)->state);
    /* クライアントの次の PUT は 404 */
    iso_upload_write_t w;
    TEST_ASSERT_EQUAL_INT(ISO_RES_NOT_FOUND, put_range(id, 5000, 10, &w));
    /* 別のアップロードを始められる */
    char id2[9];
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_upload_begin(up(), "next", 4, SZ, NULL, id2));
    TEST_ASSERT_EQUAL_INT(1, drive(0, 1000));
    TEST_ASSERT_NULL(find(id));
    TEST_ASSERT_EQUAL_STRING(id2, iso_upload_slot_id(up())); /* 削除は、他のイメージの枠に触らない */
}

/* ---- DELETE の拒否 ----------------------------------------------------------------------- */

void test_delete_of_a_missing_image_is_not_found(void) {
    TEST_ASSERT_EQUAL_INT(ISO_RES_NOT_FOUND, iso_core_submit_delete(&CORE, "000000ff"));
    TEST_ASSERT_EQUAL_INT(ISO_RES_NOT_FOUND, iso_core_submit_delete(&CORE, "../../etc"));
    TEST_ASSERT_EQUAL_INT(ISO_RES_NOT_FOUND, iso_core_submit(&CORE, ISO_JOB_IDLE_CLOSE, "000000ff"));
    TEST_ASSERT_EQUAL_INT(ISO_RES_NOT_FOUND, iso_core_submit(&CORE, ISO_JOB_INDEX, "000000ff"));
    TEST_ASSERT_EQUAL_INT(ISO_RES_NOT_FOUND, iso_core_submit_finish(&CORE, "000000ff", NULL));
}

void test_delete_of_the_mounted_image_is_refused_without_side_effects(void) {
    make_image("000000a1", "x1", SZ, PB_SMALL, ISO_META_READY, SZ);
    scan();
    TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_core_submit(&CORE, ISO_JOB_INDEX, "000000a1"));
    drive(0, 1000);
    snprintf(MOUNTED, sizeof(MOUNTED), "000000a1");
    fx_settle();
    TEST_ASSERT_EQUAL_INT(ISO_RES_MOUNTED, iso_core_submit_delete(&CORE, "000000a1"));
    TEST_ASSERT_EQUAL_INT(ISO_STATE_READY, find("000000a1")->state);
    TEST_ASSERT_EQUAL_UINT(0, iso_core_queue_len(&CORE));
    TEST_ASSERT_NOT_NULL(iso_core_table(&CORE, "000000a1"));
    TEST_ASSERT_EQUAL_INT(0, FS.nlog);
    /* 取り外した後は受理 */
    MOUNTED[0] = '\0';
    TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_core_submit_delete(&CORE, "000000a1"));
}

void test_mounted_image_that_is_not_the_target_does_not_block_delete(void) {
    make_image("000000a1", "x1", SZ, PB_SMALL, ISO_META_READY, SZ);
    make_image("000000a2", "x2", SZ, PB_SMALL, ISO_META_INCOMPLETE, 1000);
    scan();
    snprintf(MOUNTED, sizeof(MOUNTED), "000000a1");
    TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_core_submit_delete(&CORE, "000000a2"));
}

void test_delete_of_an_image_with_corrupt_meta_is_accepted(void) {
    TEST_ASSERT_EQUAL_INT(ISO_OK, mock_fs_add_dir(&FS, "/iso/000000f1"));
    scan();
    TEST_ASSERT_FALSE(find("000000f1")->meta_valid);
    TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_core_submit_delete(&CORE, "000000f1"));
    TEST_ASSERT_EQUAL_INT(1, drive(0, 1000));
    TEST_ASSERT_NULL(find("000000f1"));
    TEST_ASSERT_FALSE(mock_fs_exists(&FS, "/iso/000000f1"));
}

/* ---- INDEX の集合 ------------------------------------------------------------------------ */

void test_index_submission_requires_a_ready_image(void) {
    make_image("000000a1", "x1", SZ, PB_SMALL, ISO_META_INCOMPLETE, SZ);
    scan();
    TEST_ASSERT_EQUAL_INT(ISO_RES_NOT_UPLOADING, iso_core_submit(&CORE, ISO_JOB_INDEX, "000000a1"));
    TEST_ASSERT_EQUAL_UINT(0, iso_core_index_pending(&CORE));
}

void test_index_pick_skips_images_that_are_no_longer_ready(void) {
    make_image("000000a1", "x1", SZ, PB_SMALL, ISO_META_READY, SZ);
    make_image("000000a2", "x2", SZ, PB_SMALL, ISO_META_READY, SZ);
    scan();
    TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_core_submit(&CORE, ISO_JOB_INDEX, "000000a1"));
    TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_core_submit(&CORE, ISO_JOB_INDEX, "000000a2"));
    find("000000a1")->state = ISO_STATE_INVALID; /* 選ばれる前に、状態が変わった */
    iso_job_t *j = iso_core_pick(&CORE);
    TEST_ASSERT_NOT_NULL(j);
    TEST_ASSERT_EQUAL_STRING("000000a2", j->id);
    TEST_ASSERT_EQUAL_UINT(0, iso_core_index_pending(&CORE));
}

void test_mark_unindexed_marks_ready_images_without_a_table(void) {
    make_image("000000a1", "x1", SZ, PB_SMALL, ISO_META_READY, SZ);
    make_image("000000a2", "x2", SZ, PB_SMALL, ISO_META_READY, SZ);
    make_image("000000a3", "x3", SZ, PB_SMALL, ISO_META_INCOMPLETE, 1000);
    scan();
    TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_core_submit(&CORE, ISO_JOB_INDEX, "000000a1"));
    drive(0, 1000);
    TEST_ASSERT_NOT_NULL(iso_core_table(&CORE, "000000a1"));
    iso_core_mark_unindexed(&CORE);
    TEST_ASSERT_EQUAL_UINT(1, iso_core_index_pending(&CORE)); /* a2 だけ(a1 は表がある、a3 は ready でない) */
    iso_core_mark_unindexed(&CORE);
    TEST_ASSERT_EQUAL_UINT(1, iso_core_index_pending(&CORE)); /* 重複しない */
    /* 動いている INDEX のイメージは入れない */
    iso_job_t *j = iso_core_pick(&CORE);
    TEST_ASSERT_EQUAL_STRING("000000a2", j->id);
    iso_core_mark_unindexed(&CORE);
    TEST_ASSERT_EQUAL_UINT(0, iso_core_index_pending(&CORE));
    run_job(j, 0, 1000, 100, NULL);
}

/* ---- 不正な呼び出し ------------------------------------------------------------------------- */

/* キューが満杯(ISO_WORKER_QUEUE 件)のときに、受理された PUT が IDLE_CLOSE を取り除こうとしても、配列の外を読まない */
void test_a_put_with_a_full_queue_leaves_the_other_jobs_alone(void) {
    make_ten_incomplete();
    scan();
    for (int i = 0; i < (int)ISO_WORKER_QUEUE; i++) {
        char qid[9];
        snprintf(qid, sizeof(qid), "000001%02d", i);
        TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_core_submit(&CORE, ISO_JOB_IDLE_CLOSE, qid));
    }
    TEST_ASSERT_EQUAL_UINT(ISO_WORKER_QUEUE, iso_core_queue_len(&CORE));
    char id[9];
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_upload_begin(up(), "live", 4, 34816, NULL, id));
    put_all(id, 0, 3000, 3000);
    TEST_ASSERT_EQUAL_UINT(ISO_WORKER_QUEUE, iso_core_queue_len(&CORE)); /* 別のイメージのジョブは、そのまま */
}

void test_step_with_a_wrong_job_pointer_is_an_error(void) {
    iso_job_t other;
    memset(&other, 0, sizeof(other));
    iso_step_result_t r = iso_job_step(&CORE, &other, 0);
    TEST_ASSERT_EQUAL_INT(ISO_STEP_ERROR, r.kind);
    TEST_ASSERT_EQUAL_INT(ISO_ERR_INVALID, r.err);
    r = iso_job_step(&CORE, NULL, 0);
    TEST_ASSERT_EQUAL_INT(ISO_STEP_ERROR, r.kind);
    r = iso_job_step(NULL, &other, 0);
    TEST_ASSERT_EQUAL_INT(ISO_STEP_ERROR, r.kind);
}

void test_step_with_a_foreign_pointer_does_not_touch_the_running_job(void) {
    make_image("000000a1", "x1", SZ, PB_SMALL, ISO_META_READY, SZ);
    scan();
    TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_core_submit(&CORE, ISO_JOB_INDEX, "000000a1"));
    iso_job_t *j = iso_core_pick(&CORE);
    TEST_ASSERT_NOT_NULL(j);
    iso_job_t copy = *j; /* 動いているジョブと同じ中身の、別のオブジェクト */
    iso_step_result_t r = iso_job_step(&CORE, &copy, 0);
    TEST_ASSERT_EQUAL_INT(ISO_STEP_ERROR, r.kind);
    TEST_ASSERT_EQUAL_INT(ISO_ERR_INVALID, r.err);
    TEST_ASSERT_EQUAL_PTR(j, iso_core_pick(&CORE)); /* 動いているジョブは、そのまま(まだ終わっていない) */
    TEST_ASSERT_EQUAL_INT(ISO_JOB_INDEX, j->kind);
    r = run_job(j, 0, 1000, 100, NULL); /* 本物は、最後まで進められる */
    TEST_ASSERT_EQUAL_INT(ISO_STEP_DONE, r.kind);
    TEST_ASSERT_EQUAL_INT(ISO_STATE_READY, find("000000a1")->state);
}

void test_submit_of_an_unknown_kind_is_invalid(void) {
    TEST_ASSERT_EQUAL_INT(ISO_ERR_INVALID, iso_core_submit(&CORE, ISO_JOB_NONE, "000000a1"));
    TEST_ASSERT_EQUAL_INT(ISO_ERR_INVALID, iso_core_submit(&CORE, (iso_job_kind_t)99, "000000a1"));
}

void test_waiting_flag_of_a_null_job_is_false(void) {
    TEST_ASSERT_FALSE(iso_job_waiting(NULL));
}

void test_init_validates_the_config(void) {
    iso_core_t c;
    iso_core_cfg_t cfg;
    fx_core_cfg(&cfg, 0, 0);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_core_init(&c, &cfg));
    TEST_ASSERT_EQUAL_INT(ISO_ERR_INVALID, iso_core_init(NULL, &cfg));
    TEST_ASSERT_EQUAL_INT(ISO_ERR_INVALID, iso_core_init(&c, NULL));
    cfg.cat = NULL;
    TEST_ASSERT_EQUAL_INT(ISO_ERR_INVALID, iso_core_init(&c, &cfg));
}


void test_a_finalizing_image_without_a_job_gets_a_job_again(void) {
    char id[9];
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_upload_begin(up(), "live", 4, 34816, NULL, id));
    put_all(id, 0, 34816, 20000);
    TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_upload_finish_begin(up(), id, NULL)); /* ジョブを積まずに finalizing(あってはならない状態) */
    TEST_ASSERT_EQUAL_UINT(0, iso_core_queue_len(&CORE));
    TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_core_submit_finish(&CORE, id, NULL)); /* 冪等ではなく、積み直す */
    TEST_ASSERT_EQUAL_UINT(1, iso_core_queue_len(&CORE));
    TEST_ASSERT_EQUAL_INT(1, drive(0, 1000));
    TEST_ASSERT_EQUAL_INT(ISO_STATE_READY, find(id)->state);
}

void test_a_finalizing_image_without_a_job_and_a_full_queue_is_busy_and_unchanged(void) {
    make_ten_incomplete();
    scan();
    char id[9];
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_upload_begin(up(), "live", 4, 34816, NULL, id));
    put_all(id, 0, 34816, 20000);
    TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_upload_finish_begin(up(), id, NULL));
    for (int i = 0; i < 8; i++) {
        char qid[9];
        snprintf(qid, sizeof(qid), "000001%02d", i);
        TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_core_submit(&CORE, ISO_JOB_IDLE_CLOSE, qid));
    }
    TEST_ASSERT_EQUAL_INT(ISO_ERR_BUSY, iso_core_submit_finish(&CORE, id, NULL));
    TEST_ASSERT_EQUAL_INT(ISO_STATE_FINALIZING, find(id)->state);
    TEST_ASSERT_TRUE(up()->slot_finalizing); /* 受付を取り消さない(もともと finalizing) */
    TEST_ASSERT_TRUE(iso_upload_slot_busy(up()));
}

void test_config_chunk_and_sync_sizes_reach_the_upload(void) {
    iso_core_cfg_t cfg;
    fx_core_cfg(&cfg, 5000, 3000);
    iso_core_t c2;
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_core_init(&c2, &cfg));
    TEST_ASSERT_EQUAL_UINT32(5000, iso_upload_chunk_bytes(iso_core_upload(&c2)));
    TEST_ASSERT_EQUAL_UINT32(3000, iso_core_upload(&c2)->cfg.sync_bytes);
    TEST_ASSERT_EQUAL_UINT32(131072, iso_upload_chunk_bytes(up())); /* 0 は既定 */
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_pick_with_nothing_queued_returns_null);
    RUN_TEST(test_priority_is_delete_finish_idle_close_index);
    RUN_TEST(test_a_job_submitted_later_with_higher_priority_goes_first);
    RUN_TEST(test_pick_returns_the_running_job_until_it_ends);
    RUN_TEST(test_index_jobs_run_one_at_a_time_in_the_order_they_were_added);
    RUN_TEST(test_index_jobs_do_not_take_queue_slots);
    RUN_TEST(test_queue_holds_8_jobs_and_the_9th_is_busy);
    RUN_TEST(test_overflow_of_delete_changes_nothing);
    RUN_TEST(test_overflow_of_finish_rolls_back_an_incomplete_image);
    RUN_TEST(test_overflow_of_finish_rolls_back_an_uploading_image);
    RUN_TEST(test_finish_replaces_a_waiting_idle_close_even_when_the_queue_is_full);
    RUN_TEST(test_duplicate_submissions_join_the_existing_job);
    RUN_TEST(test_running_job_absorbs_duplicates);
    RUN_TEST(test_finish_replaces_a_waiting_idle_close_of_the_same_image);
    RUN_TEST(test_idle_close_joins_a_waiting_finish_of_the_same_image);
    RUN_TEST(test_delete_aborts_the_running_finish_job_of_the_same_image);
    RUN_TEST(test_delete_of_another_image_does_not_abort_the_running_job);
    RUN_TEST(test_delete_cancels_the_waiting_jobs_of_the_same_image);
    RUN_TEST(test_delete_aborts_the_running_index_job);
    RUN_TEST(test_delete_of_an_uploading_image_aborts_the_upload);
    RUN_TEST(test_delete_of_a_missing_image_is_not_found);
    RUN_TEST(test_delete_of_the_mounted_image_is_refused_without_side_effects);
    RUN_TEST(test_mounted_image_that_is_not_the_target_does_not_block_delete);
    RUN_TEST(test_delete_of_an_image_with_corrupt_meta_is_accepted);
    RUN_TEST(test_index_submission_requires_a_ready_image);
    RUN_TEST(test_index_pick_skips_images_that_are_no_longer_ready);
    RUN_TEST(test_mark_unindexed_marks_ready_images_without_a_table);
    RUN_TEST(test_a_put_with_a_full_queue_leaves_the_other_jobs_alone);
    RUN_TEST(test_step_with_a_wrong_job_pointer_is_an_error);
    RUN_TEST(test_step_with_a_foreign_pointer_does_not_touch_the_running_job);
    RUN_TEST(test_submit_of_an_unknown_kind_is_invalid);
    RUN_TEST(test_waiting_flag_of_a_null_job_is_false);
    RUN_TEST(test_a_finalizing_image_without_a_job_gets_a_job_again);
    RUN_TEST(test_a_finalizing_image_without_a_job_and_a_full_queue_is_busy_and_unchanged);
    RUN_TEST(test_config_chunk_and_sync_sizes_reach_the_upload);
    RUN_TEST(test_init_validates_the_config);
    return UNITY_END();
}
