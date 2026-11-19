/*
 * iso_upload_write_chunk / idle_close の失敗(設計 3.1、3.4、WEB_API 8.3): 書き込み・sync・open の失敗は、チャンク全体を truncate で取り消して
 * incomplete にする。truncate 自体の失敗では、received は実ファイルの大きさから決める。空きなし(ISO_ERR_NO_SPACE)は 507 相当。
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

void test_truncate_failure_takes_received_from_the_file_size(void) {
    begin_small(SZ);
    put_all(ID, 0, 2000, 2000);
    fx_settle();
    mock_fs_fail_write(&FS, 1, 300, ISO_ERR_NO_SPACE);
    mock_fs_error_at(&FS, 3, ISO_ERR_IO); /* write(1)、close(2)、truncate(3) */
    iso_upload_write_t w;
    TEST_ASSERT_EQUAL_INT(ISO_RES_INSUFFICIENT_STORAGE, put_range(ID, 2000, 1000, &w));
    /* 取り消せなかった: ディスク上のファイルは 2300 バイト。received はそれ */
    TEST_ASSERT_EQUAL_UINT64(2300, w.received);
    expect_incomplete_and_free(2300);
    TEST_ASSERT_EQUAL_INT64(2300, mock_fs_size(&FS, P0));
    /* SHA-256 の状態は、2000 バイトまで。received と合わないので、使わない */
    TEST_ASSERT_EQUAL_UINT64(2000, UP.sha_bytes);
    /* 再開: 追記の先頭バイトは、ソースの 2300 バイト目(書けた 300 バイトは、ソースの先頭の続き) */
    fx_settle();
    iso_upload_write_t w2;
    TEST_ASSERT_EQUAL_INT(ISO_OK, put_range(ID, 2300, 500, &w2));
    TEST_ASSERT_EQUAL_UINT64(2800, w2.received);
    TEST_ASSERT_FALSE(UP.sha_valid); /* 欠けた区間があるので、以後は逐次の計算を続けない */
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_upload_idle_close(&UP, ID));
    expect_disk_prefix(ID, PB_SMALL, 2800);
}

void test_truncate_and_stat_failures_keep_the_old_received_until_the_resume(void) {
    begin_small(SZ);
    put_all(ID, 0, 2000, 2000);
    fx_settle();
    mock_fs_fail_write(&FS, 1, 300, ISO_ERR_NO_SPACE);
    mock_fs_error_at(&FS, 3, ISO_ERR_IO);
    mock_fs_fail_path(&FS, P0); /* 大きさも読めない */
    iso_upload_write_t w;
    TEST_ASSERT_EQUAL_INT(ISO_RES_INSUFFICIENT_STORAGE, put_range(ID, 2000, 1000, &w));
    TEST_ASSERT_EQUAL_UINT64(2000, w.received); /* 分からないので、書く前の値のまま */
    expect_incomplete_and_free(2000);
    mock_fs_fail_path(&FS, NULL);
    /* 再開の最初の PUT が、ディスクから決め直す: 実際は 2300 */
    TEST_ASSERT_EQUAL_INT(ISO_RES_OFFSET_MISMATCH, put_range(ID, 2000, 100, &w));
    TEST_ASSERT_EQUAL_UINT64(2300, w.received);
    TEST_ASSERT_EQUAL_INT(ISO_OK, put_range(ID, 2300, 100, &w));
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
    RUN_TEST(test_truncate_failure_takes_received_from_the_file_size);
    RUN_TEST(test_truncate_and_stat_failures_keep_the_old_received_until_the_resume);
    RUN_TEST(test_meta_write_failure_after_the_rollback_still_makes_the_upload_incomplete_in_memory);
    RUN_TEST(test_failure_on_a_resumed_upload_leaves_it_incomplete_and_the_slot_free);
    RUN_TEST(test_idle_close_sync_failure_still_ends_incomplete);
    RUN_TEST(test_idle_close_meta_failure_still_frees_the_slot);
    RUN_TEST(test_resume_after_a_failed_idle_close_works);
    return UNITY_END();
}
