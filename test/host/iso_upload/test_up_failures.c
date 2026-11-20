/*
 * iso_upload_write_chunk / idle_close の失敗(設計 3.1、3.4、WEB_API 8.3): 書き込み・sync・open の失敗は、チャンク全体を truncate で取り消して
 * incomplete にする。truncate 自体の失敗では、received は書く前の値のまま、取り消せなかった分は保留して、次の再開と finish の受付の前に、もう一度 truncate する。
 * 空きなし(ISO_ERR_NO_SPACE)は 507 相当。
 * 失敗の注入は、モックの write の失敗(mock_fs_fail_write)と、書き込みの通し番号によるエラー(mock_fs_error_at)。期待値は手書き。
 * 電源断は test_up_powercut.c。
 */
#include "up_common.h"

void setUp(void) { fx_up_setup(0, 0); }
void tearDown(void) { fx_up_teardown(); }

static void reconf(uint32_t chunk, uint32_t sync) {
    fx_up_teardown();
    fx_up_setup(chunk, sync);
}

#define SZ 100000ull

static char ID[ISO_ID_LEN + 1];
static const char *P0 = "/iso/11110001/p000";
static const char *P1 = "/iso/11110001/p001";

static iso_catalog_entry_t *entry(void) { return iso_catalog_find(&CAT, ID); }

static void begin_small(uint64_t size) {
    img_begin_pb("img", size, PB_SMALL, NULL, ID);
    TEST_ASSERT_EQUAL_STRING("11110001", ID);
}

/* 再起動の後と同じ見え方(別のカタログでスキャン)で、状態と received を返す */
static void scan_state(iso_state_t *st, uint64_t *received) {
    iso_catalog_t c2;
    iso_catalog_init(&c2, &IFS, &IRND);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_catalog_scan(&c2));
    iso_catalog_entry_t *e = iso_catalog_find(&c2, ID);
    TEST_ASSERT_NOT_NULL(e);
    *st = e->state;
    *received = e->received;
}

static void expect_incomplete_and_free(uint64_t received) {
    TEST_ASSERT_EQUAL_INT(ISO_STATE_INCOMPLETE, entry()->state);
    TEST_ASSERT_EQUAL_UINT64(received, entry()->received);
    TEST_ASSERT_FALSE(iso_upload_slot_busy(&UP));
    TEST_ASSERT_EQUAL_INT(0, FS.open_files);
}

/* ---- write の失敗 -------------------------------------------------------------------------- */

void test_first_write_failure_is_rolled_back_and_the_upload_becomes_incomplete(void) {
    begin_small(SZ);
    fx_settle();
    mock_fs_fail_write(&FS, 1, 0, ISO_ERR_IO);
    iso_upload_write_t w;
    TEST_ASSERT_EQUAL_INT(ISO_ERR_IO, put_range(ID, 0, 1000, &w));
    TEST_ASSERT_EQUAL_UINT64(0, w.received);
    expect_incomplete_and_free(0);
    TEST_ASSERT_EQUAL_INT(4, FS.nlog);
    TEST_ASSERT_TRUE(mock_fs_log_is(&FS, 0, "open_w /iso/11110001/p000"));
    TEST_ASSERT_TRUE(mock_fs_log_is(&FS, 1, "write /iso/11110001/p000 len=1000"));
    TEST_ASSERT_TRUE(mock_fs_log_is(&FS, 2, "truncate /iso/11110001/p000 len=0")); /* 書く前の大きさ */
    TEST_ASSERT_TRUE(mock_fs_log_is(&FS, 3, "overwrite /iso/11110001/meta.a len=256")); /* incomplete を保存 */
    iso_slots_t s;
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_slots_read(&IFS, ID, &s));
    TEST_ASSERT_TRUE(s.adopted >= 0);
    TEST_ASSERT_EQUAL_UINT8(ISO_META_INCOMPLETE, s.meta[s.adopted].state);
    TEST_ASSERT_TRUE(UP.sha_valid); /* received と合っているので、SHA-256 の状態は残る */
    TEST_ASSERT_EQUAL_UINT64(0, UP.sha_bytes);
    TEST_ASSERT_EQUAL_UINT64(0, UP.consumed_since); /* 書けなかった分は、使った分に数えない */
}

void test_resume_after_a_failed_first_write_reuses_the_empty_part_file(void) {
    begin_small(SZ);
    mock_fs_fail_write(&FS, 1, 0, ISO_ERR_IO);
    iso_upload_write_t w;
    TEST_ASSERT_EQUAL_INT(ISO_ERR_IO, put_range(ID, 0, 1000, &w));
    fx_settle();
    TEST_ASSERT_EQUAL_INT(ISO_OK, put_range(ID, 0, 1000, &w)); /* p000(空)がすでにある: EXISTS → 空にして追記 */
    TEST_ASSERT_TRUE(mock_fs_log_is(&FS, 0, "open_w /iso/11110001/p000"));
    TEST_ASSERT_TRUE(mock_fs_log_is(&FS, 1, "truncate /iso/11110001/p000 len=0"));
    TEST_ASSERT_TRUE(mock_fs_log_is(&FS, 2, "open_a /iso/11110001/p000"));
    TEST_ASSERT_TRUE(mock_fs_log_is(&FS, 3, "write /iso/11110001/p000 len=1000"));
    TEST_ASSERT_EQUAL_INT(ISO_STATE_UPLOADING, entry()->state);
    TEST_ASSERT_EQUAL_UINT64(1000, UP.sha_bytes);
}

void test_partial_write_with_no_space_is_truncated_back_and_reports_507(void) {
    begin_small(SZ);
    put_all(ID, 0, 2000, 2000);
    fx_settle();
    mock_fs_fail_write(&FS, 1, 300, ISO_ERR_NO_SPACE); /* 300 バイト書けて、空きなし */
    iso_upload_write_t w;
    TEST_ASSERT_EQUAL_INT(ISO_RES_INSUFFICIENT_STORAGE, put_range(ID, 2000, 1000, &w));
    TEST_ASSERT_EQUAL_UINT64(2000, w.received);
    expect_incomplete_and_free(2000);
    TEST_ASSERT_EQUAL_INT(4, FS.nlog);
    TEST_ASSERT_TRUE(mock_fs_log_is(&FS, 0, "write /iso/11110001/p000 len=1000"));
    TEST_ASSERT_TRUE(mock_fs_log_is(&FS, 1, "close /iso/11110001/p000"));
    TEST_ASSERT_TRUE(mock_fs_log_is(&FS, 2, "truncate /iso/11110001/p000 len=2000"));
    TEST_ASSERT_TRUE(mock_fs_log_is(&FS, 3, "overwrite /iso/11110001/meta.a len=256"));
    TEST_ASSERT_EQUAL_INT64(2000, mock_fs_size(&FS, P0));
    expect_disk_prefix(ID, PB_SMALL, 2000);
    TEST_ASSERT_TRUE(UP.sha_valid);
    TEST_ASSERT_EQUAL_UINT64(2000, UP.sha_bytes);
    TEST_ASSERT_EQUAL_UINT64(2000, UP.consumed_since);
}

void test_other_write_errors_are_passed_through(void) {
    begin_small(SZ);
    mock_fs_fail_write(&FS, 1, 0, ISO_ERR_NO_CARD);
    iso_upload_write_t w;
    TEST_ASSERT_EQUAL_INT(ISO_ERR_NO_CARD, put_range(ID, 0, 100, &w));
    TEST_ASSERT_EQUAL_INT(ISO_STATE_INCOMPLETE, entry()->state);
}

void test_failure_in_the_second_part_rolls_back_the_whole_chunk(void) {
    reconf(40000, 0);
    begin_small(SZ);
    put_all(ID, 0, 30000, 30000);
    fx_settle();
    mock_fs_fail_write(&FS, 2, 100, ISO_ERR_IO); /* p000 への書き込み(1 回目)は成功、p001(2 回目)は 100 バイトで失敗 */
    iso_upload_write_t w;
    TEST_ASSERT_EQUAL_INT(ISO_ERR_IO, put_range(ID, 30000, 10000, &w));
    TEST_ASSERT_EQUAL_UINT64(30000, w.received);
    expect_incomplete_and_free(30000);
    const char *want[] = {"write /iso/11110001/p000 len=4816", "sync /iso/11110001/p000", "open_w /iso/11110001/p001",
                          "write /iso/11110001/p001 len=5184", "close /iso/11110001/p001", "truncate /iso/11110001/p001 len=0",
                          "truncate /iso/11110001/p000 len=30000", "overwrite /iso/11110001/meta.a len=256"};
    TEST_ASSERT_EQUAL_INT(8, FS.nlog);
    for (int i = 0; i < 8; i++) TEST_ASSERT_TRUE(mock_fs_log_is(&FS, i, want[i]));
    TEST_ASSERT_EQUAL_INT64(30000, mock_fs_size(&FS, P0));
    TEST_ASSERT_EQUAL_INT64(0, mock_fs_size(&FS, P1));
    expect_disk_prefix(ID, PB_SMALL, 30000);
    /* 再開して、同じチャンクを送り直せる(p001 は空で残っている) */
    fx_settle();
    TEST_ASSERT_EQUAL_INT(ISO_OK, put_range(ID, 30000, 10000, &w));
    TEST_ASSERT_TRUE(mock_fs_log_is(&FS, 0, "open_a /iso/11110001/p000"));
    TEST_ASSERT_TRUE(mock_fs_log_is(&FS, 1, "write /iso/11110001/p000 len=4816"));
    TEST_ASSERT_TRUE(mock_fs_log_is(&FS, 2, "sync /iso/11110001/p000"));
    TEST_ASSERT_TRUE(mock_fs_log_is(&FS, 3, "open_w /iso/11110001/p001"));
    TEST_ASSERT_TRUE(mock_fs_log_is(&FS, 4, "truncate /iso/11110001/p001 len=0"));
    TEST_ASSERT_TRUE(mock_fs_log_is(&FS, 5, "open_a /iso/11110001/p001"));
    TEST_ASSERT_TRUE(mock_fs_log_is(&FS, 6, "write /iso/11110001/p001 len=5184"));
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_upload_idle_close(&UP, ID));
    expect_disk_prefix(ID, PB_SMALL, 40000);
}

/* ---- sync、open の失敗 ------------------------------------------------------------------- */

void test_periodic_sync_failure_rolls_back_the_chunk(void) {
    reconf(0, 1000);
    begin_small(SZ);
    put_all(ID, 0, 600, 600);
    fx_settle();
    mock_fs_error_at(&FS, 2, ISO_ERR_IO); /* 書き込み(1)の次の sync(2)が失敗 */
    iso_upload_write_t w;
    TEST_ASSERT_EQUAL_INT(ISO_ERR_IO, put_range(ID, 600, 600, &w));
    TEST_ASSERT_EQUAL_UINT64(600, w.received);
    expect_incomplete_and_free(600);
    TEST_ASSERT_EQUAL_INT64(600, mock_fs_size(&FS, P0));
    expect_disk_prefix(ID, PB_SMALL, 600);
    TEST_ASSERT_EQUAL_UINT64(600, UP.consumed_since);
}

void test_sync_failure_at_the_end_of_a_part_rolls_back(void) {
    begin_small(SZ);
    put_all(ID, 0, 34000, 34000);
    fx_settle();
    mock_fs_error_at(&FS, 2, ISO_ERR_IO); /* write(1)、sync(2) */
    iso_upload_write_t w;
    TEST_ASSERT_EQUAL_INT(ISO_ERR_IO, put_range(ID, 34000, 816, &w));
    expect_incomplete_and_free(34000);
    TEST_ASSERT_EQUAL_INT64(34000, mock_fs_size(&FS, P0));
    TEST_ASSERT_FALSE(mock_fs_exists(&FS, P1));
}

void test_open_failure_of_the_next_part_rolls_back_the_first_part(void) {
    reconf(40000, 0);
    begin_small(SZ);
    put_all(ID, 0, 30000, 30000);
    fx_settle();
    mock_fs_error_at(&FS, 3, ISO_ERR_IO); /* write p000(1)、sync(2)、open p001(3) */
    iso_upload_write_t w;
    TEST_ASSERT_EQUAL_INT(ISO_ERR_IO, put_range(ID, 30000, 10000, &w));
    expect_incomplete_and_free(30000);
    TEST_ASSERT_EQUAL_INT64(30000, mock_fs_size(&FS, P0)); /* p000 は書く前の大きさに戻った */
    TEST_ASSERT_FALSE(mock_fs_exists(&FS, P1));
    expect_disk_prefix(ID, PB_SMALL, 30000);
}

void test_open_failure_on_the_first_chunk(void) {
    begin_small(SZ);
    fx_settle();
    mock_fs_error_at(&FS, 1, ISO_ERR_IO);
    iso_upload_write_t w;
    TEST_ASSERT_EQUAL_INT(ISO_ERR_IO, put_range(ID, 0, 100, &w));
    expect_incomplete_and_free(0);
    TEST_ASSERT_FALSE(mock_fs_exists(&FS, P0));
    TEST_ASSERT_EQUAL_INT(0, FS.n_truncate); /* 触ったパーツがない */
}

/* ---- truncate の失敗 ------------------------------------------------------------------------ */

void test_truncate_failure_keeps_the_confirmed_received_and_retries_at_the_resume(void) {
    begin_small(SZ);
    put_all(ID, 0, 2000, 2000);
    fx_settle();
    mock_fs_fail_write(&FS, 1, 300, ISO_ERR_NO_SPACE);
    mock_fs_error_at(&FS, 3, ISO_ERR_IO); /* write(1)、close(2)、truncate(3) */
    iso_upload_write_t w;
    TEST_ASSERT_EQUAL_INT(ISO_RES_INSUFFICIENT_STORAGE, put_range(ID, 2000, 1000, &w));
    /* 取り消せなかった: ディスクには、確認していない 300 バイトが残る。received は、書く前の 2000 のまま */
    TEST_ASSERT_EQUAL_UINT64(2000, w.received);
    expect_incomplete_and_free(2000);
    TEST_ASSERT_EQUAL_INT64(2300, mock_fs_size(&FS, P0));
    TEST_ASSERT_EQUAL_UINT(1, UP.pend_n); /* 次の操作で、もう一度 truncate する */
    /* 残りうる分(書こうとした 1000)を、空き容量の計上に足す: 2000(成功分)+ 1000 */
    TEST_ASSERT_EQUAL_UINT64(3000, UP.consumed_since);
    TEST_ASSERT_EQUAL_UINT64(2000, UP.sha_bytes); /* received と合っているので、SHA-256 は続けられる */
    /* 再開: 最初の PUT が、先に truncate し直す(2300 → 2000)。そのあと、2000 から追記 */
    fx_settle();
    iso_upload_write_t w2;
    TEST_ASSERT_EQUAL_INT(ISO_OK, put_range(ID, 2000, 500, &w2));
    TEST_ASSERT_EQUAL_UINT64(2500, w2.received);
    TEST_ASSERT_EQUAL_UINT(0, UP.pend_n);
    TEST_ASSERT_TRUE(mock_fs_log_is(&FS, 0, "truncate /iso/11110001/p000 len=2000"));
    TEST_ASSERT_TRUE(UP.sha_valid);
    TEST_ASSERT_EQUAL_UINT64(2500, UP.sha_bytes);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_upload_idle_close(&UP, ID));
    expect_disk_prefix(ID, PB_SMALL, 2500);
}

void test_a_pending_truncate_that_fails_again_refuses_the_resume_and_changes_nothing(void) {
    begin_small(SZ);
    put_all(ID, 0, 2000, 2000);
    fx_settle();
    mock_fs_fail_write(&FS, 1, 300, ISO_ERR_NO_SPACE);
    mock_fs_error_at(&FS, 3, ISO_ERR_IO);
    iso_upload_write_t w;
    TEST_ASSERT_EQUAL_INT(ISO_RES_INSUFFICIENT_STORAGE, put_range(ID, 2000, 1000, &w));
    mock_fs_reset_counter(&FS);
    mock_fs_error_at(&FS, 1, ISO_ERR_IO); /* 再開の最初の truncate も失敗 */
    TEST_ASSERT_EQUAL_INT(ISO_ERR_IO, put_range(ID, 2000, 100, &w));
    expect_incomplete_and_free(2000);
    TEST_ASSERT_EQUAL_UINT(1, UP.pend_n); /* 残る。次の機会に */
    TEST_ASSERT_EQUAL_INT64(2300, mock_fs_size(&FS, P0));
    mock_fs_reset_counter(&FS);
    TEST_ASSERT_EQUAL_INT(ISO_OK, put_range(ID, 2000, 100, &w));
    TEST_ASSERT_EQUAL_UINT(0, UP.pend_n);
}

void test_stray_bytes_after_a_failed_cross_part_rollback_are_not_counted_for_finish(void) {
    /* 35816 バイトのイメージ、パーツ 34816 バイト。2 つ目のパーツを含むチャンクが失敗し、両方のパーツの truncate も失敗する */
    reconf(0, 0);
    begin_small(35816);
    put_all(ID, 0, 34000, 34000);
    fx_settle();
    mock_fs_fail_write(&FS, 2, 1000, ISO_ERR_IO); /* p000 への 816 バイトは成功、p001 への 1000 バイトは書けたが、エラー */
    mock_fs_error_range(&FS, 6, 7, ISO_ERR_IO);   /* write(1)、sync(2)、open(3)、write(4 = 失敗)、close の同期(5)、truncate p001(6)と p000(7)が、どちらも失敗 */
    iso_upload_write_t w;
    int rc = put_range(ID, 34000, 1816, &w);
    TEST_ASSERT_TRUE(rc == ISO_ERR_IO);
    TEST_ASSERT_EQUAL_UINT64(34000, w.received);
    mock_fs_reset_counter(&FS);
    /* ここで、ディスクには 34816 + 1000 = 35816(size と同じ)が残っているかもしれない。finish は、受け取っていない分を数えない */
    uint64_t rec = 0;
    int fb = iso_upload_finish_begin(&UP, ID, &rec);
    TEST_ASSERT_EQUAL_INT(ISO_RES_SIZE_MISMATCH, fb);
    TEST_ASSERT_EQUAL_UINT64(34000, rec);
    TEST_ASSERT_EQUAL_UINT(0, UP.pend_n);
    TEST_ASSERT_EQUAL_INT64(34000, mock_fs_size(&FS, P0)); /* 保留の truncate を、先に済ませた */
    TEST_ASSERT_TRUE(mock_fs_size(&FS, P1) <= 0);
    /* 続きを送って完了できる */
    put_all(ID, 34000, 35816, 1816);
    TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_upload_finish_begin(&UP, ID, &rec));
}

/* 保留が複数あり、先の truncate が成功して後の truncate が失敗したとき: 成功した分は維持し、残りを次の試行に持ち越す。新しい PUT には進まない */
void test_a_partly_settled_pending_list_keeps_what_was_done_and_carries_the_rest_over(void) {
    reconf(0, 0);
    begin_small(35816);
    put_all(ID, 0, 34000, 34000);
    fx_settle();
    mock_fs_fail_write(&FS, 2, 1000, ISO_ERR_IO);
    mock_fs_error_range(&FS, 6, 7, ISO_ERR_IO); /* 取り消しの truncate(p001、p000)が、どちらも失敗 */
    iso_upload_write_t w;
    TEST_ASSERT_EQUAL_INT(ISO_ERR_IO, put_range(ID, 34000, 1816, &w));
    TEST_ASSERT_EQUAL_UINT(2, UP.pend_n);
    mock_fs_fail_write(&FS, 0, 0, 0);
    mock_fs_reset_counter(&FS);
    mock_fs_error_at(&FS, 2, ISO_ERR_IO); /* 再開の 1 つ目の truncate(p001)は成功、2 つ目(p000)が失敗 */
    TEST_ASSERT_EQUAL_INT(ISO_ERR_IO, put_range(ID, 34000, 100, &w));
    TEST_ASSERT_EQUAL_UINT(1, UP.pend_n);                  /* 済んだ分は、保留から消えている */
    TEST_ASSERT_EQUAL_STRING(ID, UP.pend[0].id);
    TEST_ASSERT_EQUAL_UINT32(0, UP.pend[0].part);          /* 残りは p000 */
    TEST_ASSERT_TRUE(mock_fs_size(&FS, P1) <= 0);          /* p001 の truncate は維持されている */
    TEST_ASSERT_EQUAL_INT(ISO_STATE_INCOMPLETE, entry()->state); /* 新しい PUT には進んでいない */
    TEST_ASSERT_FALSE(iso_upload_slot_busy(&UP));
    TEST_ASSERT_EQUAL_UINT64(34000, entry()->received);
    TEST_ASSERT_EQUAL_INT(0, FS.open_files);
    mock_fs_reset_counter(&FS);
    TEST_ASSERT_EQUAL_INT(ISO_OK, put_range(ID, 34000, 100, &w)); /* 次の試行で、残りを片づけて進む */
    TEST_ASSERT_EQUAL_UINT(0, UP.pend_n);
    TEST_ASSERT_EQUAL_UINT64(34100, w.received);
}

/* IDLE_CLOSE で sync も received の再取得も失敗し、メモリの値のまま incomplete になった後、I/O が回復して finish が呼ばれたとき:
 * ディスクの大きさで received を訂正し、size_mismatch で拒否しても、ビューを 1 回だけ更新する */
void test_finish_begin_notifies_the_view_once_when_it_corrects_received_and_rejects(void) {
    begin_small(SZ);
    put_all(ID, 0, 3000, 1000); /* 未同期のまま */
    fx_settle();
    mock_fs_error_range(&FS, 1, 2, ISO_ERR_IO); /* sync と close が失敗 */
    mock_fs_fail_path(&FS, P0);                 /* 大きさの再取得(stat)も失敗 */
    TEST_ASSERT_EQUAL_INT(ISO_ERR_IO, iso_upload_idle_close(&UP, ID));
    mock_fs_fail_path(&FS, NULL);
    mock_fs_reset_counter(&FS);
    TEST_ASSERT_EQUAL_INT(ISO_STATE_INCOMPLETE, entry()->state);
    TEST_ASSERT_EQUAL_UINT64(3000, entry()->received); /* 訂正できなかった(メモリの値) */
    int v = VIEW_CALLS;
    uint64_t rec = 0;
    TEST_ASSERT_EQUAL_INT(ISO_RES_SIZE_MISMATCH, iso_upload_finish_begin(&UP, ID, &rec));
    TEST_ASSERT_EQUAL_UINT64(0, rec); /* ディスクは 0(同期できていない) */
    TEST_ASSERT_EQUAL_UINT64(0, entry()->received);
    TEST_ASSERT_EQUAL_INT(v + 1, VIEW_CALLS);
    TEST_ASSERT_EQUAL_INT(ISO_RES_SIZE_MISMATCH, iso_upload_finish_begin(&UP, ID, &rec));
    TEST_ASSERT_EQUAL_INT(v + 1, VIEW_CALLS); /* 訂正がなければ、更新しない */
}

/* 訂正のないときは、拒否しても受理しても、ビューの更新は増えない(受理は take_slot の 1 回だけ) */
void test_finish_begin_without_a_correction_does_not_add_a_view_update(void) {
    begin_small(SZ);
    put_all(ID, 0, 3000, 1000); /* uploading のまま */
    int v = VIEW_CALLS;
    uint64_t rec = 0;
    TEST_ASSERT_EQUAL_INT(ISO_RES_SIZE_MISMATCH, iso_upload_finish_begin(&UP, ID, &rec));
    TEST_ASSERT_EQUAL_UINT64(3000, rec);
    TEST_ASSERT_EQUAL_INT(v, VIEW_CALLS);
}

void test_finish_begin_that_corrects_received_and_accepts_notifies_once(void) {
    begin_small(SZ);
    put_all(ID, 0, SZ, 20000);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_upload_idle_close(&UP, ID)); /* ディスクは SZ */
    entry()->received = 123; /* メモリの値が、小さくずれている */
    int v = VIEW_CALLS;
    uint64_t rec = 0;
    TEST_ASSERT_EQUAL_INT(ISO_RES_OK, iso_upload_finish_begin(&UP, ID, &rec));
    TEST_ASSERT_EQUAL_UINT64(SZ, rec);
    TEST_ASSERT_EQUAL_INT(v + 1, VIEW_CALLS);
}

void test_meta_write_failure_after_the_rollback_still_makes_the_upload_incomplete_in_memory(void) {
    begin_small(SZ);
    put_all(ID, 0, 2000, 2000);
    fx_settle();
    mock_fs_fail_write(&FS, 1, 0, ISO_ERR_IO);
    mock_fs_error_at(&FS, 4, ISO_ERR_IO); /* write(1)、close(2)、truncate(3)、meta(4) */
    iso_upload_write_t w;
    TEST_ASSERT_EQUAL_INT(ISO_ERR_IO, put_range(ID, 2000, 1000, &w));
    expect_incomplete_and_free(2000);
    iso_state_t st;
    uint64_t rec;
    scan_state(&st, &rec); /* meta は uploading のまま = 再起動で incomplete */
    TEST_ASSERT_EQUAL_INT(ISO_STATE_INCOMPLETE, st);
    TEST_ASSERT_EQUAL_UINT64(2000, rec);
}

/* ---- 再開の最初の PUT での失敗 --------------------------------------------------------------- */

void test_failure_on_a_resumed_upload_leaves_it_incomplete_and_the_slot_free(void) {
    begin_small(SZ);
    put_all(ID, 0, 2000, 2000);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_upload_idle_close(&UP, ID));
    fx_settle();
    mock_fs_fail_write(&FS, 1, 0, ISO_ERR_IO);
    iso_upload_write_t w;
    TEST_ASSERT_EQUAL_INT(ISO_ERR_IO, put_range(ID, 2000, 1000, &w));
    expect_incomplete_and_free(2000);
    TEST_ASSERT_EQUAL_INT64(2000, mock_fs_size(&FS, P0));
    mock_fs_fail_write(&FS, 0, 0, 0);
    TEST_ASSERT_EQUAL_INT(ISO_OK, put_range(ID, 2000, 1000, &w)); /* もう一度再開できる */
    TEST_ASSERT_EQUAL_UINT64(3000, w.received);
}

/* ---- idle_close の失敗 ---------------------------------------------------------------------- */

void test_idle_close_sync_failure_still_ends_incomplete(void) {
    begin_small(SZ);
    put_all(ID, 0, 3000, 1000);
    fx_settle();
    mock_fs_error_at(&FS, 1, ISO_ERR_IO); /* sync が失敗(close が同期する) */
    TEST_ASSERT_EQUAL_INT(ISO_ERR_IO, iso_upload_idle_close(&UP, ID));
    expect_incomplete_and_free(3000); /* received はディスクの大きさ */
    TEST_ASSERT_EQUAL_INT64(3000, mock_fs_size(&FS, P0));
    iso_state_t st;
    uint64_t rec;
    scan_state(&st, &rec);
    TEST_ASSERT_EQUAL_INT(ISO_STATE_INCOMPLETE, st);
    TEST_ASSERT_EQUAL_UINT64(3000, rec);
}

void test_idle_close_meta_failure_still_frees_the_slot(void) {
    begin_small(SZ);
    put_all(ID, 0, 3000, 1000);
    fx_settle();
    mock_fs_error_at(&FS, 2, ISO_ERR_IO); /* sync(1)、meta(2) */
    TEST_ASSERT_EQUAL_INT(ISO_ERR_IO, iso_upload_idle_close(&UP, ID));
    expect_incomplete_and_free(3000);
    iso_slots_t s;
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_slots_read(&IFS, ID, &s));
    TEST_ASSERT_TRUE(s.adopted >= 0);
    TEST_ASSERT_EQUAL_UINT8(ISO_META_UPLOADING, s.meta[s.adopted].state); /* ディスク上は uploading のまま */
    iso_state_t st;
    uint64_t rec;
    scan_state(&st, &rec);
    TEST_ASSERT_EQUAL_INT(ISO_STATE_INCOMPLETE, st);
}

void test_resume_after_a_failed_idle_close_works(void) {
    begin_small(SZ);
    put_all(ID, 0, 3000, 1000);
    mock_fs_error_at(&FS, 2, ISO_ERR_IO);
    TEST_ASSERT_EQUAL_INT(ISO_ERR_IO, iso_upload_idle_close(&UP, ID));
    mock_fs_reset_counter(&FS);
    iso_upload_write_t w;
    TEST_ASSERT_EQUAL_INT(ISO_OK, put_range(ID, 3000, 1000, &w));
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_upload_idle_close(&UP, ID));
    expect_disk_prefix(ID, PB_SMALL, 4000);
}


void test_a_clean_rollback_does_not_read_the_disk_again(void) {
    begin_small(SZ);
    put_all(ID, 0, 2000, 2000);
    fx_settle();
    FS.n_stat = 0;
    int v = VIEW_CALLS;
    mock_fs_fail_write(&FS, 1, 300, ISO_ERR_NO_SPACE);
    iso_upload_write_t w;
    TEST_ASSERT_EQUAL_INT(ISO_RES_INSUFFICIENT_STORAGE, put_range(ID, 2000, 1000, &w));
    TEST_ASSERT_EQUAL_INT(0, FS.n_stat);      /* truncate が成功したので、大きさを調べ直さない */
    TEST_ASSERT_EQUAL_INT(v + 1, VIEW_CALLS); /* 失敗の後始末で、ビューを更新する */
    TEST_ASSERT_EQUAL_UINT64(2000, w.received);
}

void test_a_failed_rollback_does_not_read_the_disk_and_is_kept_pending(void) {
    begin_small(SZ);
    put_all(ID, 0, 2000, 2000);
    fx_settle();
    FS.n_stat = 0;
    mock_fs_fail_write(&FS, 1, 300, ISO_ERR_NO_SPACE);
    mock_fs_error_at(&FS, 3, ISO_ERR_IO);
    iso_upload_write_t w;
    TEST_ASSERT_EQUAL_INT(ISO_RES_INSUFFICIENT_STORAGE, put_range(ID, 2000, 1000, &w));
    TEST_ASSERT_EQUAL_INT(0, FS.n_stat); /* 大きさは読まない(書く前の received を使う) */
    TEST_ASSERT_EQUAL_UINT(1, UP.pend_n);
}

void test_abort_and_reinit_forget_the_pending_truncates(void) {
    begin_small(SZ);
    put_all(ID, 0, 2000, 2000);
    mock_fs_fail_write(&FS, 1, 300, ISO_ERR_NO_SPACE);
    mock_fs_error_at(&FS, 3, ISO_ERR_IO);
    iso_upload_write_t w;
    TEST_ASSERT_EQUAL_INT(ISO_RES_INSUFFICIENT_STORAGE, put_range(ID, 2000, 1000, &w));
    mock_fs_reset_counter(&FS);
    TEST_ASSERT_EQUAL_UINT(1, UP.pend_n);
    iso_upload_abort(&UP, "deadbeef"); /* ほかのイメージ: 触らない */
    TEST_ASSERT_EQUAL_UINT(1, UP.pend_n);
    iso_upload_abort(&UP, ID); /* 枠を持たなくても、保留は捨てる */
    TEST_ASSERT_EQUAL_UINT(0, UP.pend_n);
    /* 保留を捨てたので、次の再開は、ディスクの 2300(取り消せなかった 300 バイトを含む)を received にする */
    TEST_ASSERT_EQUAL_INT(ISO_RES_OFFSET_MISMATCH, put_range(ID, 2000, 1000, &w));
    TEST_ASSERT_EQUAL_UINT64(2300, w.received);
    fx_settle();
    mock_fs_reset_counter(&FS);
    mock_fs_fail_write(&FS, 1, 300, ISO_ERR_NO_SPACE);
    mock_fs_error_at(&FS, 4, ISO_ERR_IO); /* open(1)、write(2)、close(3)、truncate(4) */
    TEST_ASSERT_EQUAL_INT(ISO_RES_INSUFFICIENT_STORAGE, put_range(ID, 2300, 1000, &w));
    mock_fs_reset_counter(&FS);
    TEST_ASSERT_EQUAL_UINT(1, UP.pend_n);
    iso_upload_reinit(&UP);
    TEST_ASSERT_EQUAL_UINT(0, UP.pend_n);
}

/* 1 つの PUT を、書き込みが 300 バイトで失敗し、その取り消し(truncate)も失敗するように、仕掛けて実行する(枠を持つイメージの 2000 バイト目から) */
static void failed_put_with_a_failed_rollback(const char *id) {
    mock_fs_reset_counter(&FS);
    mock_fs_fail_write(&FS, 1, 300, ISO_ERR_NO_SPACE);
    mock_fs_error_at(&FS, 3, ISO_ERR_IO); /* write(1)、close(2)、truncate(3) */
    iso_upload_write_t w;
    TEST_ASSERT_EQUAL_INT(ISO_RES_INSUFFICIENT_STORAGE, put_range(id, 2000, 1000, &w));
    mock_fs_reset_counter(&FS);
}

void test_pending_truncates_of_two_images_are_kept_apart(void) {
    begin_small(SZ); /* 11110001 */
    put_all(ID, 0, 2000, 2000);
    failed_put_with_a_failed_rollback(ID);
    TEST_ASSERT_EQUAL_UINT(1, UP.pend_n);
    char id2[ISO_ID_LEN + 1];
    img_begin_pb("img2", SZ, PB_SMALL, NULL, id2); /* 枠は空いている */
    TEST_ASSERT_EQUAL_STRING("11110002", id2);
    put_all(id2, 0, 2000, 2000);
    failed_put_with_a_failed_rollback(id2);
    TEST_ASSERT_EQUAL_UINT(2, UP.pend_n);
    /* 2 つ目のイメージの再開は、1 つ目の保留を飛ばして、自分の分だけを行う */
    iso_upload_write_t w;
    TEST_ASSERT_EQUAL_INT(ISO_OK, put_range(id2, 2000, 500, &w));
    TEST_ASSERT_EQUAL_UINT(1, UP.pend_n);
    TEST_ASSERT_EQUAL_STRING(ID, UP.pend[0].id);
    TEST_ASSERT_EQUAL_INT64(2300, mock_fs_size(&FS, P0)); /* 1 つ目は、まだ取り消していない */
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_upload_idle_close(&UP, id2));
    TEST_ASSERT_EQUAL_INT(ISO_OK, put_range(ID, 2000, 500, &w));
    TEST_ASSERT_EQUAL_UINT(0, UP.pend_n);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_upload_idle_close(&UP, ID));
    TEST_ASSERT_EQUAL_INT64(2500, mock_fs_size(&FS, P0)); /* 2000 に戻してから、500 バイト追記 */
}

/* 保留は、全イメージで共有する。他のイメージの保留が、ISO_PEND_MAX - 1 件(= 全イメージ × 全パーツから、1 件を引いた数)あっても、
 * 新しい保留は捨てない(捨てると、そのイメージの次の再開と finish が、truncate をせずに、残った未受理のバイトを数える) */
static void fill_pending_with_other_images(size_t n) {
    for (size_t i = 0; i < n; i++) {
        snprintf(UP.pend[i].id, sizeof(UP.pend[i].id), "dead%04u", (unsigned)(i / ISO_MAX_PARTS));
        UP.pend[i].part = (uint32_t)(i % ISO_MAX_PARTS);
        UP.pend[i].size = 0;
    }
    UP.pend_n = n;
}

void test_a_new_pending_truncate_is_never_dropped_even_when_the_list_is_nearly_full(void) {
    begin_small(SZ);
    put_all(ID, 0, 2000, 2000);
    fill_pending_with_other_images(ISO_PEND_MAX - 1);
    failed_put_with_a_failed_rollback(ID);
    TEST_ASSERT_EQUAL_UINT(ISO_PEND_MAX, UP.pend_n);
    TEST_ASSERT_EQUAL_STRING(ID, UP.pend[ISO_PEND_MAX - 1].id); /* 末尾に、覚えている */
    TEST_ASSERT_EQUAL_UINT32(0, UP.pend[ISO_PEND_MAX - 1].part);
    TEST_ASSERT_EQUAL_UINT64(2000, UP.pend[ISO_PEND_MAX - 1].size);
    expect_incomplete_and_free(2000);
    TEST_ASSERT_EQUAL_INT64(2300, mock_fs_size(&FS, P0));
    /* 再開は、先に truncate して(末尾の保留を、他のイメージの保留の後ろから見つけて)、2000 から続けられる。pend_settle の詰め直しは、配列の外を読まない */
    iso_upload_write_t w;
    TEST_ASSERT_EQUAL_INT(ISO_OK, put_range(ID, 2000, 500, &w));
    TEST_ASSERT_EQUAL_UINT64(2500, w.received);
    TEST_ASSERT_EQUAL_UINT(ISO_PEND_MAX - 1, UP.pend_n);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_upload_idle_close(&UP, ID));
    TEST_ASSERT_EQUAL_INT64(2500, mock_fs_size(&FS, P0));
    UP.pend_n = 0;
}

/* 防御の確認: 到達しない状態(保留が満杯)でも、配列の外には書かない。正常な動作では、イメージごとに ISO_MAX_PARTS 件以下で、満杯にならない */
void test_the_guard_of_a_full_pending_list_never_writes_past_the_array(void) {
    begin_small(SZ);
    put_all(ID, 0, 2000, 2000);
    fill_pending_with_other_images(ISO_PEND_MAX);
    failed_put_with_a_failed_rollback(ID);
    TEST_ASSERT_EQUAL_UINT(ISO_PEND_MAX, UP.pend_n);
    for (size_t i = 0; i < UP.pend_n; i++) TEST_ASSERT_TRUE(strncmp(UP.pend[i].id, "dead", 4) == 0);
    expect_incomplete_and_free(2000);
    UP.pend_n = 0;
}

void test_the_pending_list_holds_every_part_of_every_image(void) {
    /* 全イメージ(ISO_MAX_IMAGES)× 全パーツ(ISO_MAX_PARTS)の保留が、そろっても、どれも捨てない */
    begin_small(SZ);
    put_all(ID, 0, 2000, 2000);
    fill_pending_with_other_images(ISO_PEND_MAX - ISO_MAX_PARTS);
    /* 1 つのチャンクで、ISO_MAX_PARTS 個のパーツを触って、全部の取り消しが失敗する、ことを、保留を直接足して表す */
    for (size_t k = 0; k < ISO_MAX_PARTS; k++) {
        snprintf(UP.pend[UP.pend_n].id, sizeof(UP.pend[0].id), "%s", ID);
        UP.pend[UP.pend_n].part = (uint32_t)k;
        UP.pend[UP.pend_n].size = 0;
        UP.pend_n++;
    }
    TEST_ASSERT_EQUAL_UINT(ISO_PEND_MAX, UP.pend_n);
    TEST_ASSERT_EQUAL_UINT(ISO_MAX_IMAGES * ISO_MAX_PARTS, ISO_PEND_MAX);
    iso_upload_abort(&UP, ID);
    TEST_ASSERT_EQUAL_UINT(ISO_PEND_MAX - ISO_MAX_PARTS, UP.pend_n); /* このイメージの分だけ、捨てる */
    UP.pend_n = 0;
}

void test_finish_begin_settles_the_pending_truncates_first_and_stops_when_that_fails(void) {
    begin_small(SZ);
    put_all(ID, 0, 2000, 2000);
    failed_put_with_a_failed_rollback(ID);
    TEST_ASSERT_EQUAL_UINT(1, UP.pend_n);
    mock_fs_error_at(&FS, 1, ISO_ERR_IO); /* 保留の truncate が、また失敗する */
    uint64_t rec = 12345;
    TEST_ASSERT_EQUAL_INT(ISO_ERR_IO, iso_upload_finish_begin(&UP, ID, &rec));
    TEST_ASSERT_EQUAL_UINT64(12345, rec); /* 何も決めない */
    TEST_ASSERT_EQUAL_INT(ISO_STATE_INCOMPLETE, entry()->state);
    TEST_ASSERT_EQUAL_UINT(1, UP.pend_n);
    mock_fs_reset_counter(&FS);
    TEST_ASSERT_EQUAL_INT(ISO_RES_SIZE_MISMATCH, iso_upload_finish_begin(&UP, ID, &rec)); /* 今度は済む */
    TEST_ASSERT_EQUAL_UINT64(2000, rec);
    TEST_ASSERT_EQUAL_UINT(0, UP.pend_n);
}

void test_idle_close_when_sync_and_close_both_fail_takes_received_from_the_disk(void) {
    begin_small(SZ);
    put_all(ID, 0, 3000, 1000); /* 未同期のまま */
    fx_settle();
    mock_fs_error_range(&FS, 1, 2, ISO_ERR_IO); /* sync と、close の同期が失敗 */
    int v = VIEW_CALLS;
    TEST_ASSERT_EQUAL_INT(ISO_ERR_IO, iso_upload_idle_close(&UP, ID));
    TEST_ASSERT_EQUAL_INT(v + 1, VIEW_CALLS);
    expect_incomplete_and_free(0); /* ディレクトリ上の大きさは 0(同期できていない)。メモリの 3000 ではない */
    mock_fs_reset_counter(&FS);
    /* 再開: 0 から送り直せる(空でないかもしれない p000 は、EXISTS で 0 に切り詰める) */
    iso_upload_write_t w;
    TEST_ASSERT_EQUAL_INT(ISO_RES_OFFSET_MISMATCH, put_range(ID, 3000, 10, &w));
    TEST_ASSERT_EQUAL_UINT64(0, w.received);
    put_all(ID, 0, 3000, 1000);
    TEST_ASSERT_FALSE(UP.sha_valid); /* 受け取った分と合わなくなった SHA-256 の状態は、捨てる(finish が読み直す) */
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_upload_idle_close(&UP, ID));
    expect_disk_prefix(ID, PB_SMALL, 3000);
}

void test_idle_close_reports_a_failing_close_after_a_good_sync(void) {
    begin_small(SZ);
    put_all(ID, 0, 3000, 1000);
    mock_fs_set_fail_close(&FS, true);
    TEST_ASSERT_EQUAL_INT(ISO_ERR_IO, iso_upload_idle_close(&UP, ID)); /* sync は成功、close が失敗 */
    mock_fs_set_fail_close(&FS, false);
    expect_incomplete_and_free(3000);
    TEST_ASSERT_EQUAL_INT64(3000, mock_fs_size(&FS, P0));
}

void test_the_end_of_a_part_reports_a_failing_close(void) {
    begin_small(SZ);
    put_all(ID, 0, 34000, 34000);
    fx_settle();
    mock_fs_set_fail_close(&FS, true);
    iso_upload_write_t w;
    TEST_ASSERT_EQUAL_INT(ISO_ERR_IO, put_range(ID, 34000, 816, &w)); /* パーツの終わりの sync + close。close の失敗は、書き込みの失敗 */
    mock_fs_set_fail_close(&FS, false);
    expect_incomplete_and_free(34000);
    TEST_ASSERT_EQUAL_INT64(34000, mock_fs_size(&FS, P0));
}

void test_resume_reports_an_error_when_the_part_sizes_cannot_be_read(void) {
    begin_small(SZ);
    put_all(ID, 0, 2000, 2000);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_upload_idle_close(&UP, ID));
    fx_settle();
    mock_fs_fail_path(&FS, P0);
    iso_upload_write_t w;
    TEST_ASSERT_EQUAL_INT(ISO_ERR_IO, put_range(ID, 2000, 100, &w)); /* 何も変えない */
    mock_fs_fail_path(&FS, NULL);
    TEST_ASSERT_EQUAL_INT(0, FS.nlog);
    expect_incomplete_and_free(2000);
    TEST_ASSERT_EQUAL_INT(ISO_OK, put_range(ID, 2000, 100, &w));
}

void test_a_good_idle_close_does_not_read_the_sizes_again(void) {
    begin_small(SZ);
    put_all(ID, 0, 2000, 2000);
    FS.n_stat = 0;
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_upload_idle_close(&UP, ID));
    TEST_ASSERT_EQUAL_INT(0, FS.n_stat);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_first_write_failure_is_rolled_back_and_the_upload_becomes_incomplete);
    RUN_TEST(test_resume_after_a_failed_first_write_reuses_the_empty_part_file);
    RUN_TEST(test_partial_write_with_no_space_is_truncated_back_and_reports_507);
    RUN_TEST(test_other_write_errors_are_passed_through);
    RUN_TEST(test_failure_in_the_second_part_rolls_back_the_whole_chunk);
    RUN_TEST(test_periodic_sync_failure_rolls_back_the_chunk);
    RUN_TEST(test_sync_failure_at_the_end_of_a_part_rolls_back);
    RUN_TEST(test_open_failure_of_the_next_part_rolls_back_the_first_part);
    RUN_TEST(test_open_failure_on_the_first_chunk);
    RUN_TEST(test_truncate_failure_keeps_the_confirmed_received_and_retries_at_the_resume);
    RUN_TEST(test_a_pending_truncate_that_fails_again_refuses_the_resume_and_changes_nothing);
    RUN_TEST(test_stray_bytes_after_a_failed_cross_part_rollback_are_not_counted_for_finish);
    RUN_TEST(test_a_partly_settled_pending_list_keeps_what_was_done_and_carries_the_rest_over);
    RUN_TEST(test_finish_begin_notifies_the_view_once_when_it_corrects_received_and_rejects);
    RUN_TEST(test_finish_begin_without_a_correction_does_not_add_a_view_update);
    RUN_TEST(test_finish_begin_that_corrects_received_and_accepts_notifies_once);
    RUN_TEST(test_meta_write_failure_after_the_rollback_still_makes_the_upload_incomplete_in_memory);
    RUN_TEST(test_failure_on_a_resumed_upload_leaves_it_incomplete_and_the_slot_free);
    RUN_TEST(test_idle_close_sync_failure_still_ends_incomplete);
    RUN_TEST(test_idle_close_meta_failure_still_frees_the_slot);
    RUN_TEST(test_resume_after_a_failed_idle_close_works);
    RUN_TEST(test_a_clean_rollback_does_not_read_the_disk_again);
    RUN_TEST(test_a_failed_rollback_does_not_read_the_disk_and_is_kept_pending);
    RUN_TEST(test_abort_and_reinit_forget_the_pending_truncates);
    RUN_TEST(test_pending_truncates_of_two_images_are_kept_apart);
    RUN_TEST(test_a_new_pending_truncate_is_never_dropped_even_when_the_list_is_nearly_full);
    RUN_TEST(test_the_guard_of_a_full_pending_list_never_writes_past_the_array);
    RUN_TEST(test_the_pending_list_holds_every_part_of_every_image);
    RUN_TEST(test_finish_begin_settles_the_pending_truncates_first_and_stops_when_that_fails);
    RUN_TEST(test_idle_close_when_sync_and_close_both_fail_takes_received_from_the_disk);
    RUN_TEST(test_idle_close_reports_a_failing_close_after_a_good_sync);
    RUN_TEST(test_the_end_of_a_part_reports_a_failing_close);
    RUN_TEST(test_resume_reports_an_error_when_the_part_sizes_cannot_be_read);
    RUN_TEST(test_a_good_idle_close_does_not_read_the_sizes_again);
    return UNITY_END();
}
