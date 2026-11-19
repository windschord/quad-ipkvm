/*
 * iso_catalog: 削除のステップ関数(設計 3.4 末尾)。順序: (1) meta を deleting で書く (2) p* を unlink
 * (3) 採用していない古いスロットを先に、最後に新しい方 (4) rmdir。1 回の呼び出しで 1 ファイル。期待値は手書き。
 */
#include "ic_common.h"

void setUp(void) { fx_setup(); }
void tearDown(void) { fx_teardown(); }

#define ID "00000001"

/* meta.a (世代 7、ready、採用される) と meta.b (世代 6、incomplete)、パーツ 2 つ。ディレクトリの順: meta.a、meta.b、p000、p001 */
static void make_image_two_slots_two_parts(void) {
    iso_meta_t a = mk_meta("Img", 5000000, ISO_META_READY, 7);
    iso_meta_t b = mk_meta("Img", 5000000, ISO_META_INCOMPLETE, 6);
    put_slot(ID, 0, &a);
    put_slot(ID, 1, &b);
    put_part(ID, 0, 3000000);
    put_part(ID, 1, 2000000);
}

static int run_until_done(iso_delete_job_t *job, int max_steps, iso_step_result_t *last) {
    int steps = 0;
    do {
        *last = iso_catalog_delete_step(&CAT, job);
        steps++;
    } while (last->kind == ISO_STEP_CONTINUE && steps < max_steps);
    return steps;
}

/* 1 回の呼び出しで書き込みがちょうど n 回あったか */
static void expect_mutations(int before, int n) { TEST_ASSERT_EQUAL_INT(before + n, FS.mut_count); }

void test_delete_full_sequence_and_order(void) {
    make_image_two_slots_two_parts();
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_catalog_scan(&CAT));
    iso_catalog_entry_t *e = iso_catalog_find(&CAT, ID);
    TEST_ASSERT_EQUAL_INT(0, iso_catalog_apply(&CAT, e, ISO_EV_DELETE, &(iso_trans_ctx_t){false, true, false}, NULL));
    fx_settle();
    iso_delete_job_t job;
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_delete_job_init(&job, ID));
    iso_step_result_t r;

    /* (1) meta を deleting で書く: 採用していない方 (meta.b) に、世代 8 */
    r = iso_catalog_delete_step(&CAT, &job);
    TEST_ASSERT_EQUAL_INT(ISO_STEP_CONTINUE, r.kind);
    expect_mutations(0, 1);
    TEST_ASSERT_TRUE(mock_fs_log_is(&FS, 0, "overwrite /iso/00000001/meta.b len=256"));
    iso_slots_t s;
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_slots_read(&IFS, ID, &s));
    TEST_ASSERT_EQUAL_INT8(1, s.adopted);
    TEST_ASSERT_EQUAL_UINT8(ISO_META_DELETING, s.meta[1].state);
    TEST_ASSERT_EQUAL_UINT32(8, s.meta[1].generation);
    TEST_ASSERT_EQUAL_UINT8(ISO_META_READY, s.meta[0].state); /* 古い ready はまだある(が、採用されない) */
    TEST_ASSERT_EQUAL_INT(ISO_STATE_DELETING, e->state);
    TEST_ASSERT_EQUAL_INT8(1, e->adopted);

    /* (2) p* を 1 つずつ */
    r = iso_catalog_delete_step(&CAT, &job);
    TEST_ASSERT_EQUAL_INT(ISO_STEP_CONTINUE, r.kind);
    expect_mutations(0, 2);
    TEST_ASSERT_TRUE(mock_fs_log_is(&FS, 1, "remove /iso/00000001/p000"));
    r = iso_catalog_delete_step(&CAT, &job);
    TEST_ASSERT_EQUAL_INT(ISO_STEP_CONTINUE, r.kind);
    expect_mutations(0, 3);
    TEST_ASSERT_TRUE(mock_fs_log_is(&FS, 2, "remove /iso/00000001/p001"));

    /* (3) 古い方 (meta.a) を先に、最後に deleting を書いた方 (meta.b) */
    r = iso_catalog_delete_step(&CAT, &job);
    TEST_ASSERT_EQUAL_INT(ISO_STEP_CONTINUE, r.kind);
    expect_mutations(0, 4);
    TEST_ASSERT_TRUE(mock_fs_log_is(&FS, 3, "remove /iso/00000001/meta.a"));
    r = iso_catalog_delete_step(&CAT, &job);
    TEST_ASSERT_EQUAL_INT(ISO_STEP_CONTINUE, r.kind);
    expect_mutations(0, 5);
    TEST_ASSERT_TRUE(mock_fs_log_is(&FS, 4, "remove /iso/00000001/meta.b"));

    /* (4) rmdir -> DONE。カタログからも消える */
    TEST_ASSERT_NOT_NULL(iso_catalog_find(&CAT, ID));
    r = iso_catalog_delete_step(&CAT, &job);
    TEST_ASSERT_EQUAL_INT(ISO_STEP_DONE, r.kind);
    TEST_ASSERT_EQUAL_INT(ISO_OK, r.err);
    expect_mutations(0, 6);
    TEST_ASSERT_TRUE(mock_fs_log_is(&FS, 5, "remove /iso/00000001"));
    TEST_ASSERT_FALSE(mock_fs_exists(&FS, "/iso/00000001"));
    TEST_ASSERT_NULL(iso_catalog_find(&CAT, ID));
    TEST_ASSERT_EQUAL_UINT(0, CAT.count);
    TEST_ASSERT_EQUAL_INT(6, FS.nlog);
    /* DONE の後に呼んでも、何もしない */
    r = iso_catalog_delete_step(&CAT, &job);
    TEST_ASSERT_EQUAL_INT(ISO_STEP_DONE, r.kind);
    TEST_ASSERT_EQUAL_INT(6, FS.nlog);
}

void test_delete_when_slot_a_is_the_older_one(void) {
    /* meta.b (世代 9) が採用されている: deleting は meta.a に書く。最後に消すのは deleting を書いた方 = meta.a、先に消すのは古い meta.b */
    iso_meta_t a = mk_meta("Img", 5000000, ISO_META_INCOMPLETE, 8);
    iso_meta_t b = mk_meta("Img", 5000000, ISO_META_READY, 9);
    put_slot(ID, 0, &a);
    put_slot(ID, 1, &b);
    put_part(ID, 0, 5000000);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_catalog_scan(&CAT));
    fx_settle();
    iso_delete_job_t job;
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_delete_job_init(&job, ID));
    iso_step_result_t r;
    int steps = run_until_done(&job, 20, &r);
    TEST_ASSERT_EQUAL_INT(ISO_STEP_DONE, r.kind);
    TEST_ASSERT_EQUAL_INT(5, steps); /* (1) 書く、(2) p000、(3) meta.b (古い)、meta.a (新しい)、(4) rmdir */
    TEST_ASSERT_TRUE(mock_fs_log_is(&FS, 0, "overwrite /iso/00000001/meta.a len=256"));
    TEST_ASSERT_TRUE(mock_fs_log_is(&FS, 1, "remove /iso/00000001/p000"));
    TEST_ASSERT_TRUE(mock_fs_log_is(&FS, 2, "remove /iso/00000001/meta.b"));
    TEST_ASSERT_TRUE(mock_fs_log_is(&FS, 3, "remove /iso/00000001/meta.a"));
    TEST_ASSERT_TRUE(mock_fs_log_is(&FS, 4, "remove /iso/00000001"));
    TEST_ASSERT_EQUAL_INT(5, FS.nlog);
}

void test_delete_resume_when_slot_a_holds_the_deleting_meta(void) {
    /* 採用されているのが meta.a (deleting、世代 8)、古い meta.b (ready、世代 7)。古い方 = meta.b を先に消し、最後に meta.a */
    iso_meta_t a = mk_meta("Img", 5000000, ISO_META_DELETING, 8);
    iso_meta_t b = mk_meta("Img", 5000000, ISO_META_READY, 7);
    put_slot(ID, 0, &a);
    put_slot(ID, 1, &b);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_catalog_scan(&CAT));
    fx_settle();
    iso_delete_job_t job;
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_delete_job_init(&job, ID));
    iso_step_result_t r;
    run_until_done(&job, 20, &r);
    TEST_ASSERT_EQUAL_INT(ISO_STEP_DONE, r.kind);
    TEST_ASSERT_TRUE(mock_fs_log_is(&FS, 0, "remove /iso/00000001/meta.b"));
    TEST_ASSERT_TRUE(mock_fs_log_is(&FS, 1, "remove /iso/00000001/meta.a"));
    TEST_ASSERT_TRUE(mock_fs_log_is(&FS, 2, "remove /iso/00000001"));
    TEST_ASSERT_EQUAL_INT(3, FS.nlog);
}

void test_delete_with_a_single_slot(void) {
    iso_meta_t a = mk_meta("Img", 5000000, ISO_META_READY, 1);
    put_slot(ID, 0, &a);
    put_part(ID, 0, 5000000);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_catalog_scan(&CAT));
    fx_settle();
    iso_delete_job_t job;
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_delete_job_init(&job, ID));
    iso_step_result_t r;
    run_until_done(&job, 20, &r);
    TEST_ASSERT_EQUAL_INT(ISO_STEP_DONE, r.kind);
    /* deleting は meta.b に世代 2 で書く。古い meta.a を先に消し、meta.b を最後に */
    TEST_ASSERT_TRUE(mock_fs_log_is(&FS, 0, "overwrite /iso/00000001/meta.b len=256"));
    TEST_ASSERT_TRUE(mock_fs_log_is(&FS, 2, "remove /iso/00000001/meta.a"));
    TEST_ASSERT_TRUE(mock_fs_log_is(&FS, 3, "remove /iso/00000001/meta.b"));
    TEST_ASSERT_TRUE(mock_fs_log_is(&FS, 4, "remove /iso/00000001"));
    TEST_ASSERT_FALSE(mock_fs_exists(&FS, "/iso/00000001"));
}

void test_delete_resume_when_meta_already_says_deleting(void) {
    /* 再起動後の続き: deleting の meta (b、世代 8) が採用されている。書き直さない */
    iso_meta_t a = mk_meta("Img", 5000000, ISO_META_READY, 7);
    iso_meta_t b = mk_meta("Img", 5000000, ISO_META_DELETING, 8);
    put_slot(ID, 0, &a);
    put_slot(ID, 1, &b);
    put_part(ID, 1, 2000000); /* p000 は消えている */
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_catalog_scan(&CAT));
    iso_catalog_entry_t *e = iso_catalog_find(&CAT, ID);
    TEST_ASSERT_TRUE(e->resume_delete);
    fx_settle();
    iso_delete_job_t job;
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_delete_job_init(&job, ID));
    iso_step_result_t r = iso_catalog_delete_step(&CAT, &job); /* INIT: 何も書かない */
    TEST_ASSERT_EQUAL_INT(ISO_STEP_CONTINUE, r.kind);
    TEST_ASSERT_EQUAL_INT(0, FS.nlog);
    run_until_done(&job, 20, &r);
    TEST_ASSERT_EQUAL_INT(ISO_STEP_DONE, r.kind);
    TEST_ASSERT_TRUE(mock_fs_log_is(&FS, 0, "remove /iso/00000001/p001"));
    TEST_ASSERT_TRUE(mock_fs_log_is(&FS, 1, "remove /iso/00000001/meta.a")); /* 古い方が先 */
    TEST_ASSERT_TRUE(mock_fs_log_is(&FS, 2, "remove /iso/00000001/meta.b"));
    TEST_ASSERT_TRUE(mock_fs_log_is(&FS, 3, "remove /iso/00000001"));
    TEST_ASSERT_EQUAL_INT(4, FS.nlog);
}

void test_delete_resume_with_only_the_new_slot_left(void) {
    /* 古いスロットはすでに消えている(電源断が (3) の途中)。NOT_FOUND でも先へ進む */
    iso_meta_t b = mk_meta("Img", 5000000, ISO_META_DELETING, 8);
    put_slot(ID, 1, &b);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_catalog_scan(&CAT));
    fx_settle();
    iso_delete_job_t job;
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_delete_job_init(&job, ID));
    iso_step_result_t r;
    int steps = run_until_done(&job, 20, &r);
    TEST_ASSERT_EQUAL_INT(ISO_STEP_DONE, r.kind);
    TEST_ASSERT_EQUAL_INT(4, steps); /* INIT (書かない)、meta.a (無い)、meta.b、rmdir */
    TEST_ASSERT_TRUE(mock_fs_log_is(&FS, 0, "remove /iso/00000001/meta.a"));
    TEST_ASSERT_TRUE(mock_fs_log_is(&FS, 1, "remove /iso/00000001/meta.b"));
    TEST_ASSERT_TRUE(mock_fs_log_is(&FS, 2, "remove /iso/00000001"));
    TEST_ASSERT_FALSE(mock_fs_exists(&FS, "/iso/00000001"));
}

void test_delete_meta_corrupt_directory(void) {
    /* 有効なスロットがない: deleting は書けない。a を先、b を後に消して、rmdir */
    iso_meta_t a = mk_meta("Img", 5000000, ISO_META_READY, 7);
    put_slot(ID, 0, &a);
    TEST_ASSERT_TRUE(mock_fs_set_size(&FS, "/iso/00000001/meta.a", 128)); /* 書き込み途中 */
    put_part(ID, 0, 1234);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_catalog_scan(&CAT));
    TEST_ASSERT_FALSE(CAT.images[0].meta_valid);
    fx_settle();
    iso_delete_job_t job;
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_delete_job_init(&job, ID));
    iso_step_result_t r = iso_catalog_delete_step(&CAT, &job);
    TEST_ASSERT_EQUAL_INT(ISO_STEP_CONTINUE, r.kind);
    TEST_ASSERT_EQUAL_INT(0, FS.nlog); /* INIT は何も書かない */
    run_until_done(&job, 20, &r);
    TEST_ASSERT_EQUAL_INT(ISO_STEP_DONE, r.kind);
    TEST_ASSERT_TRUE(mock_fs_log_is(&FS, 0, "remove /iso/00000001/p000"));
    TEST_ASSERT_TRUE(mock_fs_log_is(&FS, 1, "remove /iso/00000001/meta.a"));
    TEST_ASSERT_TRUE(mock_fs_log_is(&FS, 2, "remove /iso/00000001/meta.b"));
    TEST_ASSERT_TRUE(mock_fs_log_is(&FS, 3, "remove /iso/00000001"));
    TEST_ASSERT_FALSE(mock_fs_exists(&FS, "/iso/00000001"));
    TEST_ASSERT_EQUAL_UINT(0, CAT.count);
}

void test_delete_directory_already_gone_is_done(void) {
    iso_delete_job_t job;
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_delete_job_init(&job, "deadbeef"));
    iso_step_result_t r;
    run_until_done(&job, 20, &r);
    TEST_ASSERT_EQUAL_INT(ISO_STEP_DONE, r.kind);
    TEST_ASSERT_EQUAL_INT(ISO_OK, r.err);
}

void test_delete_one_mutation_per_step(void) {
    make_image_two_slots_two_parts();
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_catalog_scan(&CAT));
    fx_settle();
    iso_delete_job_t job;
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_delete_job_init(&job, ID));
    iso_step_result_t r;
    int prev = 0, steps = 0;
    do {
        r = iso_catalog_delete_step(&CAT, &job);
        steps++;
        TEST_ASSERT_EQUAL_INT_MESSAGE(prev + 1, FS.mut_count, "exactly one mutation per step");
        prev = FS.mut_count;
    } while (r.kind == ISO_STEP_CONTINUE && steps < 20);
    TEST_ASSERT_EQUAL_INT(6, steps);
}

void test_delete_only_removes_p_entries_and_fails_on_foreign_files(void) {
    make_image_two_slots_two_parts();
    TEST_ASSERT_EQUAL_INT(ISO_OK, mock_fs_add_file(&FS, "/iso/00000001/notes.txt", "x", 1, 0));
    TEST_ASSERT_EQUAL_INT(ISO_OK, mock_fs_add_file(&FS, "/iso/00000001/pfoo", "x", 1, 0)); /* 'p' で始まる = 消す */
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_catalog_scan(&CAT));
    iso_delete_job_t job;
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_delete_job_init(&job, ID));
    iso_step_result_t r;
    run_until_done(&job, 20, &r);
    TEST_ASSERT_EQUAL_INT(ISO_STEP_ERROR, r.kind); /* notes.txt が残っているので rmdir できない */
    TEST_ASSERT_EQUAL_INT(ISO_ERR_INVALID, r.err);
    TEST_ASSERT_TRUE(mock_fs_exists(&FS, "/iso/00000001/notes.txt"));
    TEST_ASSERT_FALSE(mock_fs_exists(&FS, "/iso/00000001/p000"));
    TEST_ASSERT_FALSE(mock_fs_exists(&FS, "/iso/00000001/p001"));
    TEST_ASSERT_FALSE(mock_fs_exists(&FS, "/iso/00000001/pfoo"));
    TEST_ASSERT_FALSE(mock_fs_exists(&FS, "/iso/00000001/meta.a"));
    TEST_ASSERT_FALSE(mock_fs_exists(&FS, "/iso/00000001/meta.b"));
    TEST_ASSERT_NOT_NULL(iso_catalog_find(&CAT, ID)); /* ERROR ではカタログから消さない */
}

void test_delete_error_keeps_the_phase_and_retries_the_same_operation(void) {
    make_image_two_slots_two_parts();
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_catalog_scan(&CAT));
    fx_settle();
    iso_delete_job_t job;
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_delete_job_init(&job, ID));
    iso_step_result_t r = iso_catalog_delete_step(&CAT, &job); /* (1) */
    TEST_ASSERT_EQUAL_INT(ISO_STEP_CONTINUE, r.kind);
    mock_fs_error_at(&FS, 2, ISO_ERR_IO); /* 今から 2 回目の書き込み = p001 の remove */
    r = iso_catalog_delete_step(&CAT, &job); /* p000 */
    TEST_ASSERT_EQUAL_INT(ISO_STEP_CONTINUE, r.kind);
    r = iso_catalog_delete_step(&CAT, &job); /* p001: 失敗 */
    TEST_ASSERT_EQUAL_INT(ISO_STEP_ERROR, r.kind);
    TEST_ASSERT_EQUAL_INT(ISO_ERR_IO, r.err);
    TEST_ASSERT_TRUE(mock_fs_exists(&FS, "/iso/00000001/p001"));
    TEST_ASSERT_NOT_NULL(iso_catalog_find(&CAT, ID));
    r = iso_catalog_delete_step(&CAT, &job); /* 再試行 */
    TEST_ASSERT_EQUAL_INT(ISO_STEP_CONTINUE, r.kind);
    TEST_ASSERT_FALSE(mock_fs_exists(&FS, "/iso/00000001/p001"));
    run_until_done(&job, 20, &r);
    TEST_ASSERT_EQUAL_INT(ISO_STEP_DONE, r.kind);
}

/* 6 回の書き込みのどれが失敗しても、ERROR (その書き込みのエラー)になり、同じ操作を再試行して最後まで進める */
void test_delete_error_at_every_write_is_reported_and_retried(void) {
    for (int n = 1; n <= 6; n++) {
        mock_fs_free(&FS);
        fx_setup();
        make_image_two_slots_two_parts();
        TEST_ASSERT_EQUAL_INT(ISO_OK, iso_catalog_scan(&CAT));
        fx_settle();
        mock_fs_error_at(&FS, n, ISO_ERR_NO_SPACE);
        iso_delete_job_t job;
        TEST_ASSERT_EQUAL_INT(ISO_OK, iso_delete_job_init(&job, ID));
        iso_step_result_t r;
        int steps = 0;
        do {
            r = iso_catalog_delete_step(&CAT, &job);
            steps++;
        } while (r.kind == ISO_STEP_CONTINUE && steps < 20);
        char msg[40];
        snprintf(msg, sizeof(msg), "error at write %d", n);
        TEST_ASSERT_EQUAL_INT_MESSAGE(ISO_STEP_ERROR, r.kind, msg);
        TEST_ASSERT_EQUAL_INT_MESSAGE(ISO_ERR_NO_SPACE, r.err, msg);
        TEST_ASSERT_EQUAL_INT_MESSAGE(n, steps, msg); /* 失敗したステップで止まる */
        TEST_ASSERT_NOT_NULL_MESSAGE(iso_catalog_find(&CAT, ID), msg);
        run_until_done(&job, 20, &r); /* 再試行(エラーの注入は 1 回だけ) */
        TEST_ASSERT_EQUAL_INT_MESSAGE(ISO_STEP_DONE, r.kind, msg);
        TEST_ASSERT_FALSE_MESSAGE(mock_fs_exists(&FS, "/iso/00000001"), msg);
    }
}

void test_delete_first_step_write_failure_changes_nothing(void) {
    make_image_two_slots_two_parts();
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_catalog_scan(&CAT));
    iso_catalog_entry_t *e = iso_catalog_find(&CAT, ID);
    TEST_ASSERT_EQUAL_INT(0, iso_catalog_apply(&CAT, e, ISO_EV_DELETE, &(iso_trans_ctx_t){false, true, false}, NULL));
    e->state = ISO_STATE_READY; /* 「deleting を書く前」の状態に戻して、ステップが勝手に変えないことを見る */
    fx_settle();
    mock_fs_error_at(&FS, 1, ISO_ERR_NO_SPACE);
    iso_delete_job_t job;
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_delete_job_init(&job, ID));
    iso_step_result_t r = iso_catalog_delete_step(&CAT, &job);
    TEST_ASSERT_EQUAL_INT(ISO_STEP_ERROR, r.kind);
    TEST_ASSERT_EQUAL_INT(ISO_ERR_NO_SPACE, r.err);
    TEST_ASSERT_EQUAL_INT(ISO_STATE_READY, e->state);
    TEST_ASSERT_EQUAL_INT8(0, e->adopted);
    TEST_ASSERT_EQUAL_UINT32(7, e->meta.generation);
    TEST_ASSERT_TRUE(mock_fs_exists(&FS, "/iso/00000001/p000")); /* まだ何も消していない */
    /* 再試行は最初から */
    fx_settle();
    r = iso_catalog_delete_step(&CAT, &job);
    TEST_ASSERT_EQUAL_INT(ISO_STEP_CONTINUE, r.kind);
    TEST_ASSERT_TRUE(mock_fs_log_is(&FS, 0, "overwrite /iso/00000001/meta.b len=256"));
    TEST_ASSERT_EQUAL_INT(ISO_STATE_DELETING, e->state);
}

void test_delete_read_errors_are_reported(void) {
    make_image_two_slots_two_parts();
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_catalog_scan(&CAT));
    iso_delete_job_t job;
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_delete_job_init(&job, ID));
    mock_fs_fail_path(&FS, "/iso/00000001/meta.a");
    iso_step_result_t r = iso_catalog_delete_step(&CAT, &job); /* スロットの読み出しが IO エラー */
    TEST_ASSERT_EQUAL_INT(ISO_STEP_ERROR, r.kind);
    TEST_ASSERT_EQUAL_INT(ISO_ERR_IO, r.err);
    mock_fs_fail_path(&FS, NULL);
    r = iso_catalog_delete_step(&CAT, &job); /* INIT をやり直す */
    TEST_ASSERT_EQUAL_INT(ISO_STEP_CONTINUE, r.kind);
    mock_fs_fail_dir_next(&FS, "/iso/00000001");
    r = iso_catalog_delete_step(&CAT, &job); /* 一覧の読み出しが失敗 */
    TEST_ASSERT_EQUAL_INT(ISO_STEP_ERROR, r.kind);
    TEST_ASSERT_EQUAL_INT(ISO_ERR_IO, r.err);
    mock_fs_fail_dir_next(&FS, NULL);
    mock_fs_fail_path(&FS, "/iso/00000001");
    r = iso_catalog_delete_step(&CAT, &job); /* dir_open が失敗 (NOT_FOUND ではない) */
    TEST_ASSERT_EQUAL_INT(ISO_STEP_ERROR, r.kind);
    mock_fs_fail_path(&FS, NULL);
    run_until_done(&job, 20, &r);
    TEST_ASSERT_EQUAL_INT(ISO_STEP_DONE, r.kind);
}

/* ---- クロスリンク(設計 3.3): import/ のファイルと <id>/p000 が同じ先頭クラスタ ---- */

static void make_crosslinked_image(uint32_t cluster) {
    iso_meta_t a = mk_meta("Img", 40000, ISO_META_READY, 7);
    a.part_count = 1;
    a.part_blocks = 20;
    put_slot(ID, 0, &a);
    TEST_ASSERT_EQUAL_INT(ISO_OK, mock_fs_add_sparse(&FS, "/iso/00000001/p000", 40000, NULL, 0, cluster));
    TEST_ASSERT_EQUAL_INT(ISO_OK, mock_fs_add_sparse(&FS, "/iso/import/foo.iso", 40000, NULL, 0, cluster));
}

void test_delete_precheck_refuses_when_import_has_the_same_first_cluster(void) {
    make_crosslinked_image(777);
    TEST_ASSERT_EQUAL_INT(ISO_CAT_ERR_CROSSLINKED, iso_catalog_delete_precheck(&CAT, ID));
    /* 何も変更しない */
    TEST_ASSERT_EQUAL_INT(0, FS.nlog);
    /* 別のクラスタなら OK */
    TEST_ASSERT_TRUE(mock_fs_set_first_cluster(&FS, "/iso/import/foo.iso", 778));
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_catalog_delete_precheck(&CAT, ID));
    /* 2 つ目以降のファイルとも照合する */
    TEST_ASSERT_EQUAL_INT(ISO_OK, mock_fs_add_sparse(&FS, "/iso/import/bar.iso", 40000, NULL, 0, 777));
    TEST_ASSERT_EQUAL_INT(ISO_CAT_ERR_CROSSLINKED, iso_catalog_delete_precheck(&CAT, ID));
    TEST_ASSERT_EQUAL_INT(0, FS.open_dirs);
}

void test_delete_precheck_non_matches(void) {
    make_crosslinked_image(777);
    /* 先頭クラスタ 0 は、一致と見なさない */
    TEST_ASSERT_TRUE(mock_fs_set_first_cluster(&FS, "/iso/00000001/p000", 0));
    TEST_ASSERT_TRUE(mock_fs_set_first_cluster(&FS, "/iso/import/foo.iso", 0));
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_catalog_delete_precheck(&CAT, ID));
    /* import/ のディレクトリは、クラスタが同じでも、ファイルではない */
    TEST_ASSERT_TRUE(mock_fs_set_first_cluster(&FS, "/iso/00000001/p000", 777));
    TEST_ASSERT_EQUAL_INT(ISO_OK, IFS.remove(&FS, "/iso/import/foo.iso"));
    TEST_ASSERT_EQUAL_INT(ISO_OK, mock_fs_add_dir(&FS, "/iso/import/dir.iso"));
    TEST_ASSERT_TRUE(mock_fs_set_first_cluster(&FS, "/iso/import/dir.iso", 777));
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_catalog_delete_precheck(&CAT, ID));
    /* p000 がない(まだアップロード前など)、p000 がディレクトリ */
    TEST_ASSERT_EQUAL_INT(ISO_OK, mock_fs_add_sparse(&FS, "/iso/import/same.iso", 40000, NULL, 0, 777));
    TEST_ASSERT_EQUAL_INT(ISO_CAT_ERR_CROSSLINKED, iso_catalog_delete_precheck(&CAT, ID));
    TEST_ASSERT_EQUAL_INT(ISO_OK, IFS.remove(&FS, "/iso/00000001/p000"));
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_catalog_delete_precheck(&CAT, ID));
    TEST_ASSERT_EQUAL_INT(ISO_OK, mock_fs_add_dir(&FS, "/iso/00000001/p000"));
    TEST_ASSERT_TRUE(mock_fs_set_first_cluster(&FS, "/iso/00000001/p000", 777));
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_catalog_delete_precheck(&CAT, ID));
    /* import/ がない */
    TEST_ASSERT_EQUAL_INT(ISO_OK, IFS.remove(&FS, "/iso/00000001/p000"));
    TEST_ASSERT_EQUAL_INT(ISO_OK, mock_fs_add_sparse(&FS, "/iso/00000001/p000", 40000, NULL, 0, 777));
    TEST_ASSERT_EQUAL_INT(ISO_OK, IFS.remove(&FS, "/iso/import/same.iso"));
    TEST_ASSERT_EQUAL_INT(ISO_OK, IFS.remove(&FS, "/iso/import/dir.iso"));
    TEST_ASSERT_EQUAL_INT(ISO_OK, IFS.remove(&FS, "/iso/import"));
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_catalog_delete_precheck(&CAT, ID));
    /* 引数 */
    TEST_ASSERT_EQUAL_INT(ISO_ERR_INVALID, iso_catalog_delete_precheck(&CAT, "../x"));
    TEST_ASSERT_EQUAL_INT(ISO_ERR_INVALID, iso_catalog_delete_precheck(NULL, ID));
}

void test_delete_precheck_propagates_io_errors(void) {
    make_crosslinked_image(777);
    mock_fs_fail_path(&FS, "/iso/00000001/p000");
    TEST_ASSERT_EQUAL_INT(ISO_ERR_IO, iso_catalog_delete_precheck(&CAT, ID));
    mock_fs_fail_path(&FS, "/iso/import");
    TEST_ASSERT_EQUAL_INT(ISO_ERR_IO, iso_catalog_delete_precheck(&CAT, ID));
    mock_fs_fail_path(&FS, NULL);
    mock_fs_fail_dir_next(&FS, "/iso/import");
    TEST_ASSERT_EQUAL_INT(ISO_ERR_IO, iso_catalog_delete_precheck(&CAT, ID));
    mock_fs_fail_dir_next(&FS, NULL);
    TEST_ASSERT_EQUAL_INT(ISO_CAT_ERR_CROSSLINKED, iso_catalog_delete_precheck(&CAT, ID));
}

void test_delete_step_refuses_a_crosslinked_image_without_changing_anything(void) {
    make_crosslinked_image(777);
    iso_meta_t b = mk_meta("Img", 40000, ISO_META_INCOMPLETE, 6);
    b.part_count = 1;
    b.part_blocks = 20;
    put_slot(ID, 1, &b);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_catalog_scan(&CAT));
    iso_catalog_entry_t *e = iso_catalog_find(&CAT, ID);
    fx_settle();
    iso_delete_job_t job;
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_delete_job_init(&job, ID));
    for (int i = 0; i < 3; i++) { /* 何度呼んでも、同じ結果(再試行しても進まない) */
        iso_step_result_t r = iso_catalog_delete_step(&CAT, &job);
        TEST_ASSERT_EQUAL_INT(ISO_STEP_ERROR, r.kind);
        TEST_ASSERT_EQUAL_INT(ISO_CAT_ERR_CROSSLINKED, r.err);
    }
    TEST_ASSERT_EQUAL_INT(0, FS.mut_count); /* 書き込みなし(meta を deleting で書かない) */
    TEST_ASSERT_EQUAL_INT(0, FS.nlog);
    TEST_ASSERT_TRUE(mock_fs_exists(&FS, "/iso/00000001/p000"));
    TEST_ASSERT_TRUE(mock_fs_exists(&FS, "/iso/import/foo.iso"));
    TEST_ASSERT_EQUAL_INT(ISO_STATE_READY, e->state);
    TEST_ASSERT_EQUAL_INT8(0, e->adopted);
    TEST_ASSERT_EQUAL_UINT32(7, e->meta.generation);
    TEST_ASSERT_NOT_NULL(iso_catalog_find(&CAT, ID));
    /* PC で直した後(import/ のファイルがなくなった)は、削除できる */
    TEST_ASSERT_EQUAL_INT(ISO_OK, IFS.remove(&FS, "/iso/import/foo.iso"));
    iso_step_result_t r;
    run_until_done(&job, 20, &r);
    TEST_ASSERT_EQUAL_INT(ISO_STEP_DONE, r.kind);
    TEST_ASSERT_FALSE(mock_fs_exists(&FS, "/iso/00000001"));
}

void test_delete_resume_with_crosslink_keeps_p000_and_everything_else(void) {
    /* meta はすでに deleting (a: 世代 8 を採用、b: 世代 7)。p000 は import/ のファイルと同じクラスタ、p001 もある */
    iso_meta_t a = mk_meta("Img", 40000, ISO_META_DELETING, 8);
    iso_meta_t b = mk_meta("Img", 40000, ISO_META_READY, 7);
    a.part_count = b.part_count = 1;
    a.part_blocks = b.part_blocks = 20;
    put_slot(ID, 0, &a);
    put_slot(ID, 1, &b);
    TEST_ASSERT_EQUAL_INT(ISO_OK, mock_fs_add_sparse(&FS, "/iso/00000001/p000", 40000, NULL, 0, 555));
    TEST_ASSERT_EQUAL_INT(ISO_OK, mock_fs_add_sparse(&FS, "/iso/00000001/p001", 100, NULL, 0, 556));
    TEST_ASSERT_EQUAL_INT(ISO_OK, mock_fs_add_sparse(&FS, "/iso/import/foo.iso", 40000, NULL, 0, 555));
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_catalog_scan(&CAT));
    TEST_ASSERT_TRUE(CAT.images[0].resume_delete);
    fx_settle();
    iso_delete_job_t job;
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_delete_job_init(&job, ID));
    iso_step_result_t r = iso_catalog_delete_step(&CAT, &job);
    TEST_ASSERT_EQUAL_INT(ISO_STEP_ERROR, r.kind);
    TEST_ASSERT_EQUAL_INT(ISO_CAT_ERR_CROSSLINKED, r.err);
    TEST_ASSERT_EQUAL_INT(0, FS.mut_count); /* p000 も、p001 も、meta も、消さない */
    TEST_ASSERT_TRUE(mock_fs_exists(&FS, "/iso/00000001/p000"));
    TEST_ASSERT_TRUE(mock_fs_exists(&FS, "/iso/00000001/p001"));
    TEST_ASSERT_TRUE(mock_fs_exists(&FS, "/iso/00000001/meta.a"));
    TEST_ASSERT_TRUE(mock_fs_exists(&FS, "/iso/00000001/meta.b"));
    TEST_ASSERT_TRUE(mock_fs_exists(&FS, "/iso/import/foo.iso"));
    TEST_ASSERT_NOT_NULL(iso_catalog_find(&CAT, ID));
    /* 再起動して、スキャンし直しても、deleting のまま残る */
    iso_catalog_t c2;
    iso_catalog_init(&c2, &IFS, &IRND);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_catalog_scan(&c2));
    TEST_ASSERT_EQUAL_INT(ISO_STATE_DELETING, c2.images[0].state);
    TEST_ASSERT_TRUE(c2.images[0].resume_delete);
}

void test_delete_step_reports_io_errors_of_the_crosslink_check_without_changing_anything(void) {
    make_crosslinked_image(777);
    TEST_ASSERT_EQUAL_INT(ISO_OK, mock_fs_set_first_cluster(&FS, "/iso/import/foo.iso", 778) ? ISO_OK : ISO_ERR_INVALID);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_catalog_scan(&CAT));
    fx_settle();
    iso_delete_job_t job;
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_delete_job_init(&job, ID));
    mock_fs_fail_path(&FS, "/iso/00000001/p000"); /* p000 の stat が失敗 */
    iso_step_result_t r = iso_catalog_delete_step(&CAT, &job);
    TEST_ASSERT_EQUAL_INT(ISO_STEP_ERROR, r.kind);
    TEST_ASSERT_EQUAL_INT(ISO_ERR_IO, r.err);
    mock_fs_fail_path(&FS, "/iso/import"); /* import/ が開けない */
    r = iso_catalog_delete_step(&CAT, &job);
    TEST_ASSERT_EQUAL_INT(ISO_STEP_ERROR, r.kind);
    TEST_ASSERT_EQUAL_INT(ISO_ERR_IO, r.err);
    mock_fs_fail_path(&FS, NULL);
    mock_fs_fail_dir_next(&FS, "/iso/import"); /* 一覧の読み出しが失敗 */
    r = iso_catalog_delete_step(&CAT, &job);
    TEST_ASSERT_EQUAL_INT(ISO_STEP_ERROR, r.kind);
    TEST_ASSERT_EQUAL_INT(ISO_ERR_IO, r.err);
    mock_fs_fail_dir_next(&FS, NULL);
    TEST_ASSERT_EQUAL_INT(0, FS.mut_count); /* どれも、何も書いていない */
    r = iso_catalog_delete_step(&CAT, &job);
    TEST_ASSERT_EQUAL_INT(ISO_STEP_CONTINUE, r.kind); /* 失敗が消えれば進む(クロスリンクではない) */
}

void test_delete_step_ignores_p_directories_and_stops_at_rmdir(void) {
    make_image_two_slots_two_parts();
    TEST_ASSERT_EQUAL_INT(ISO_OK, mock_fs_add_dir(&FS, "/iso/00000001/p-notes")); /* p で始まるディレクトリ(外来) */
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_catalog_scan(&CAT));
    iso_delete_job_t job;
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_delete_job_init(&job, ID));
    iso_step_result_t r;
    run_until_done(&job, 20, &r);
    TEST_ASSERT_EQUAL_INT(ISO_STEP_ERROR, r.kind);
    TEST_ASSERT_TRUE(mock_fs_is_dir(&FS, "/iso/00000001/p-notes")); /* ディレクトリは消さない(rmdir が失敗する) */
    TEST_ASSERT_FALSE(mock_fs_exists(&FS, "/iso/00000001/p000"));
    TEST_ASSERT_FALSE(mock_fs_exists(&FS, "/iso/00000001/p001"));
}

void test_delete_job_init_rejects_bad_id(void) {
    iso_delete_job_t job;
    TEST_ASSERT_EQUAL_INT(ISO_ERR_INVALID, iso_delete_job_init(&job, "../etc"));
    TEST_ASSERT_EQUAL_INT(ISO_ERR_INVALID, iso_delete_job_init(&job, "DEADBEEF"));
    TEST_ASSERT_EQUAL_INT(ISO_ERR_INVALID, iso_delete_job_init(&job, NULL));
    TEST_ASSERT_EQUAL_INT(ISO_ERR_INVALID, iso_delete_job_init(NULL, "deadbeef"));
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_delete_job_init(&job, "deadbeef"));
    TEST_ASSERT_EQUAL_STRING("deadbeef", job.id);
}

void test_delete_of_one_image_leaves_others_untouched(void) {
    make_image_two_slots_two_parts();
    iso_meta_t o = mk_meta("Other", 5000000, ISO_META_READY, 1);
    put_slot("00000002", 0, &o);
    put_part("00000002", 0, 5000000);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_catalog_scan(&CAT));
    TEST_ASSERT_EQUAL_UINT(2, CAT.count);
    iso_delete_job_t job;
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_delete_job_init(&job, ID));
    iso_step_result_t r;
    run_until_done(&job, 20, &r);
    TEST_ASSERT_EQUAL_INT(ISO_STEP_DONE, r.kind);
    TEST_ASSERT_EQUAL_UINT(1, CAT.count);
    TEST_ASSERT_EQUAL_STRING("00000002", CAT.images[0].id);
    TEST_ASSERT_TRUE(mock_fs_exists(&FS, "/iso/00000002/meta.a"));
    TEST_ASSERT_TRUE(mock_fs_exists(&FS, "/iso/00000002/p000"));
}

/* 手順の (1) が終わった後は、途中のどこで電源が落ちても、ready が復活しない(設計 3.4 の「順序が重要」) */
void test_delete_never_resurrects_ready_after_step_1(void) {
    make_image_two_slots_two_parts();
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_catalog_scan(&CAT));
    iso_delete_job_t job;
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_delete_job_init(&job, ID));
    iso_step_result_t r;
    int steps = 0;
    do {
        r = iso_catalog_delete_step(&CAT, &job);
        steps++;
        /* 各ステップの後に、再起動を模擬して(別のカタログで)スキャンする */
        iso_catalog_t c2;
        iso_catalog_init(&c2, &IFS, &IRND);
        TEST_ASSERT_EQUAL_INT(ISO_OK, iso_catalog_scan(&c2));
        if (r.kind == ISO_STEP_DONE) {
            TEST_ASSERT_EQUAL_UINT(0, c2.count);
        } else {
            TEST_ASSERT_EQUAL_UINT(1, c2.count);
            /* deleting として続ける、または(最後のスロットを消した後は)meta_corrupt として削除できる。ready にはならない */
            bool deleting = (c2.images[0].state == ISO_STATE_DELETING && c2.images[0].resume_delete);
            bool corrupt = (c2.images[0].state == ISO_STATE_INVALID && !c2.images[0].meta_valid);
            TEST_ASSERT_TRUE_MESSAGE(deleting || corrupt, "after step 1 the image must never be seen as ready / incomplete");
        }
    } while (r.kind == ISO_STEP_CONTINUE && steps < 20);
    TEST_ASSERT_EQUAL_INT(ISO_STEP_DONE, r.kind);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_delete_full_sequence_and_order);
    RUN_TEST(test_delete_when_slot_a_is_the_older_one);
    RUN_TEST(test_delete_resume_when_slot_a_holds_the_deleting_meta);
    RUN_TEST(test_delete_with_a_single_slot);
    RUN_TEST(test_delete_resume_when_meta_already_says_deleting);
    RUN_TEST(test_delete_resume_with_only_the_new_slot_left);
    RUN_TEST(test_delete_meta_corrupt_directory);
    RUN_TEST(test_delete_directory_already_gone_is_done);
    RUN_TEST(test_delete_one_mutation_per_step);
    RUN_TEST(test_delete_only_removes_p_entries_and_fails_on_foreign_files);
    RUN_TEST(test_delete_error_keeps_the_phase_and_retries_the_same_operation);
    RUN_TEST(test_delete_error_at_every_write_is_reported_and_retried);
    RUN_TEST(test_delete_first_step_write_failure_changes_nothing);
    RUN_TEST(test_delete_read_errors_are_reported);
    RUN_TEST(test_delete_precheck_refuses_when_import_has_the_same_first_cluster);
    RUN_TEST(test_delete_precheck_non_matches);
    RUN_TEST(test_delete_precheck_propagates_io_errors);
    RUN_TEST(test_delete_step_refuses_a_crosslinked_image_without_changing_anything);
    RUN_TEST(test_delete_resume_with_crosslink_keeps_p000_and_everything_else);
    RUN_TEST(test_delete_step_reports_io_errors_of_the_crosslink_check_without_changing_anything);
    RUN_TEST(test_delete_step_ignores_p_directories_and_stops_at_rmdir);
    RUN_TEST(test_delete_job_init_rejects_bad_id);
    RUN_TEST(test_delete_of_one_image_leaves_others_untouched);
    RUN_TEST(test_delete_never_resurrects_ready_after_step_1);
    return UNITY_END();
}
