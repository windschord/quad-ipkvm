/*
 * iso_index_step(INDEX ジョブと FINISH ジョブが共有): パーツごとの stat + get_extents → 期待値(meta)との照合 → エクステントの表。
 * 起動時・SD の再初期化後の ready のイメージに対する INDEX(設計 3.4)。期待値は手書き。
 */
#include "up_common.h"

void setUp(void) { fx_up_setup(0, 0); }
void tearDown(void) { fx_up_teardown(); }

#define SZ 100000ull
#define ID "0000000a"

/* ready のイメージ(パーツ 3 つ。34816 + 34816 + 30368)を、ディスクに置いて、カタログをスキャンする */
static void make_ready(void) {
    iso_meta_t m = mk_meta("img", SZ, PB_SMALL, ISO_META_READY, 5);
    m.sha_flag = ISO_SHA_COMPUTED;
    put_meta(ID, &m);
    put_parts_data(ID, PB_SMALL, SZ);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_catalog_scan(&CAT));
    TEST_ASSERT_EQUAL_INT(ISO_STATE_READY, iso_catalog_find(&CAT, ID)->state);
}

static iso_step_result_t run(iso_index_job_t *job, int *steps) {
    iso_step_result_t r;
    int n = 0;
    do {
        r = iso_index_step(&UP, job);
        n++;
        TEST_ASSERT_TRUE(n < 1000);
    } while (r.kind == ISO_STEP_CONTINUE);
    if (steps) *steps = n;
    return r;
}

static void set_known_extents(void) {
    const iso_extent_t e0[] = {{1000, 68}};
    const iso_extent_t e1[] = {{5000, 68}};
    const iso_extent_t e2[] = {{9000, 60}}; /* 30368 バイト = 59.3 セクタ → 60 */
    TEST_ASSERT_TRUE(mock_fs_set_extents(&FS, "/iso/" ID "/p000", e0, 1));
    TEST_ASSERT_TRUE(mock_fs_set_extents(&FS, "/iso/" ID "/p001", e1, 1));
    TEST_ASSERT_TRUE(mock_fs_set_extents(&FS, "/iso/" ID "/p002", e2, 1));
}

void test_index_builds_the_table_part_by_part(void) {
    make_ready();
    set_known_extents();
    iso_index_job_t job;
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_index_job_init(&job, ID));
    iso_step_result_t r;
    /* 1 回目: 作業領域の確保(CONTINUE)。以降 1 回に 1 パーツ、最後に表の構築 */
    r = iso_index_step(&UP, &job);
    TEST_ASSERT_EQUAL_INT(ISO_STEP_CONTINUE, r.kind);
    TEST_ASSERT_EQUAL_INT(1, ALLOC_LIVE);
    for (int i = 0; i < 3; i++) {
        r = iso_index_step(&UP, &job);
        TEST_ASSERT_EQUAL_INT(ISO_STEP_CONTINUE, r.kind);
        TEST_ASSERT_EQUAL_UINT32((uint32_t)i + 1, job.part);
    }
    r = iso_index_step(&UP, &job);
    TEST_ASSERT_EQUAL_INT(ISO_STEP_DONE, r.kind);
    TEST_ASSERT_EQUAL_INT(ISO_OUTCOME_OK, job.outcome);
    TEST_ASSERT_NOT_NULL(job.table);
    TEST_ASSERT_NULL(job.pool); /* 作業領域は解放済み。表だけが残る */
    TEST_ASSERT_EQUAL_UINT32(1, iso_extent_table_refs(job.table));
    TEST_ASSERT_EQUAL_UINT32(EPOCH, iso_extent_table_epoch(job.table));
    TEST_ASSERT_EQUAL_UINT64(SZ, iso_extent_table_size_bytes(job.table));
    TEST_ASSERT_EQUAL_UINT64(49, iso_extent_table_capacity_blocks(job.table));
    TEST_ASSERT_EQUAL_UINT32(3, iso_extent_table_part_count(job.table));
    TEST_ASSERT_EQUAL_UINT32(PB_SMALL, iso_extent_table_part_blocks(job.table));
    TEST_ASSERT_EQUAL_UINT32(3, iso_extent_table_extent_count(job.table));
    /* 変換: 全体(49 ブロック)= 196 セクタ(100000 バイトを切り上げ)の 3 区間 */
    iso_extent_t spans[8];
    size_t n = 0;
    TEST_ASSERT_EQUAL_INT(ISO_EXT_OK, iso_extent_map(job.table, 0, 49, spans, 8, &n));
    TEST_ASSERT_EQUAL_UINT(3, n);
    TEST_ASSERT_EQUAL_UINT64(1000, spans[0].start_sector);
    TEST_ASSERT_EQUAL_UINT64(68, spans[0].sector_count);
    TEST_ASSERT_EQUAL_UINT64(5000, spans[1].start_sector);
    TEST_ASSERT_EQUAL_UINT64(68, spans[1].sector_count);
    TEST_ASSERT_EQUAL_UINT64(9000, spans[2].start_sector);
    TEST_ASSERT_EQUAL_UINT64(60, spans[2].sector_count);
    iso_index_job_release(&UP, &job); /* 取り出していない表を放す */
    TEST_ASSERT_NULL(job.table);
    TEST_ASSERT_EQUAL_INT(0, ALLOC_LIVE);
}

void test_index_step_count_is_init_plus_parts_plus_build(void) {
    make_ready();
    iso_index_job_t job;
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_index_job_init(&job, ID));
    int steps = 0;
    iso_step_result_t r = run(&job, &steps);
    TEST_ASSERT_EQUAL_INT(ISO_STEP_DONE, r.kind);
    TEST_ASSERT_EQUAL_INT(5, steps);
    TEST_ASSERT_EQUAL_INT(ISO_OUTCOME_OK, job.outcome);
    iso_index_job_release(&UP, &job);
}

void test_index_after_done_stays_done(void) {
    make_ready();
    iso_index_job_t job;
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_index_job_init(&job, ID));
    iso_step_result_t r = run(&job, NULL);
    TEST_ASSERT_EQUAL_INT(ISO_STEP_DONE, r.kind);
    r = iso_index_step(&UP, &job);
    TEST_ASSERT_EQUAL_INT(ISO_STEP_DONE, r.kind);
    TEST_ASSERT_EQUAL_INT(ISO_OUTCOME_OK, job.outcome);
    iso_index_job_release(&UP, &job);
    iso_index_job_release(&UP, &job); /* 冪等 */
}

void test_index_clips_orphan_tail_extents(void) {
    make_ready();
    /* FatFs の連鎖は、クラッシュ後の孤児の末尾クラスタまで列挙しうる。ファイルのセクタ数でクリップする */
    const iso_extent_t e0[] = {{1000, 68}, {7777, 500}};
    TEST_ASSERT_TRUE(mock_fs_set_extents(&FS, "/iso/" ID "/p000", e0, 2));
    iso_index_job_t job;
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_index_job_init(&job, ID));
    iso_step_result_t r = run(&job, NULL);
    TEST_ASSERT_EQUAL_INT(ISO_STEP_DONE, r.kind);
    TEST_ASSERT_EQUAL_INT(ISO_OUTCOME_OK, job.outcome);
    TEST_ASSERT_EQUAL_UINT32(3, iso_extent_table_extent_count(job.table));
    iso_index_job_release(&UP, &job);
}

void test_index_without_blockdev_uses_epoch_0(void) {
    make_ready();
    UP.cfg.bdev = NULL;
    iso_index_job_t job;
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_index_job_init(&job, ID));
    run(&job, NULL);
    TEST_ASSERT_EQUAL_UINT32(0, iso_extent_table_epoch(job.table));
    iso_index_job_release(&UP, &job);
}

void test_epoch_is_recorded_at_the_start(void) {
    make_ready();
    iso_index_job_t job;
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_index_job_init(&job, ID));
    iso_step_result_t r = iso_index_step(&UP, &job);
    TEST_ASSERT_EQUAL_INT(ISO_STEP_CONTINUE, r.kind);
    TEST_ASSERT_EQUAL_UINT32(7, job.epoch);
    EPOCH = 8; /* 途中でカードが替わった */
    r = run(&job, NULL);
    TEST_ASSERT_EQUAL_UINT32(7, iso_extent_table_epoch(job.table));
    iso_index_job_release(&UP, &job);
}

/* ---- 照合の失敗(invalid) ------------------------------------------------------------------- */

static void expect_invalid(iso_index_job_t *job, uint8_t reason) {
    iso_step_result_t r = run(job, NULL);
    TEST_ASSERT_EQUAL_INT(ISO_STEP_DONE, r.kind);
    TEST_ASSERT_EQUAL_INT(ISO_OUTCOME_INVALID, job->outcome);
    TEST_ASSERT_EQUAL_UINT8(reason, job->reason);
    TEST_ASSERT_NULL(job->table);
    TEST_ASSERT_NULL(job->pool);
    TEST_ASSERT_EQUAL_INT(0, ALLOC_LIVE);
}

void test_missing_part_is_part_mismatch(void) {
    make_ready();
    TEST_ASSERT_EQUAL_INT(ISO_OK, IFS.remove(&FS, "/iso/" ID "/p002"));
    iso_index_job_t job;
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_index_job_init(&job, ID));
    expect_invalid(&job, ISO_INVALID_PART_MISMATCH);
}

void test_missing_first_part_is_part_mismatch(void) {
    make_ready();
    TEST_ASSERT_EQUAL_INT(ISO_OK, IFS.remove(&FS, "/iso/" ID "/p000"));
    iso_index_job_t job;
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_index_job_init(&job, ID));
    expect_invalid(&job, ISO_INVALID_PART_MISMATCH);
}

void test_wrong_size_part_is_part_mismatch(void) {
    static const uint64_t sizes[] = {0, 1, 34815, 34817, 100000};
    for (size_t k = 0; k < sizeof(sizes) / sizeof(sizes[0]); k++) {
        fx_up_teardown();
        fx_up_setup(0, 0);
        make_ready();
        TEST_ASSERT_TRUE(mock_fs_set_size(&FS, "/iso/" ID "/p001", sizes[k]));
        iso_index_job_t job;
        TEST_ASSERT_EQUAL_INT(ISO_OK, iso_index_job_init(&job, ID));
        expect_invalid(&job, ISO_INVALID_PART_MISMATCH);
    }
}

void test_part_that_is_a_directory_is_part_mismatch(void) {
    make_ready();
    TEST_ASSERT_EQUAL_INT(ISO_OK, IFS.remove(&FS, "/iso/" ID "/p001"));
    TEST_ASSERT_EQUAL_INT(ISO_OK, mock_fs_add_dir(&FS, "/iso/" ID "/p001"));
    iso_index_job_t job;
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_index_job_init(&job, ID));
    expect_invalid(&job, ISO_INVALID_PART_MISMATCH);
}

void test_extents_that_do_not_cover_the_file_are_part_mismatch(void) {
    make_ready();
    const iso_extent_t e1[] = {{5000, 60}}; /* 68 セクタ必要 */
    TEST_ASSERT_TRUE(mock_fs_set_extents(&FS, "/iso/" ID "/p001", e1, 1));
    iso_index_job_t job;
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_index_job_init(&job, ID));
    expect_invalid(&job, ISO_INVALID_PART_MISMATCH);
}

void test_overflowing_extent_is_part_mismatch(void) {
    make_ready();
    const iso_extent_t e1[] = {{UINT64_MAX - 10, 68}};
    TEST_ASSERT_TRUE(mock_fs_set_extents(&FS, "/iso/" ID "/p001", e1, 1));
    iso_index_job_t job;
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_index_job_init(&job, ID));
    expect_invalid(&job, ISO_INVALID_PART_MISMATCH);
}

void test_get_extents_not_found_is_part_mismatch(void) {
    make_ready();
    TEST_ASSERT_TRUE(mock_fs_fail_extents(&FS, "/iso/" ID "/p001", ISO_ERR_NOT_FOUND));
    iso_index_job_t job;
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_index_job_init(&job, ID));
    expect_invalid(&job, ISO_INVALID_PART_MISMATCH);
}

void test_too_fragmented_from_the_fs(void) {
    make_ready();
    TEST_ASSERT_TRUE(mock_fs_fail_extents(&FS, "/iso/" ID "/p000", ISO_ERR_TOO_FRAGMENTED));
    iso_index_job_t job;
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_index_job_init(&job, ID));
    expect_invalid(&job, ISO_INVALID_TOO_FRAGMENTED);
}

void test_part_count_that_disagrees_with_the_meta_is_part_mismatch(void) {
    iso_meta_t m = mk_meta("img", SZ, PB_SMALL, ISO_META_READY, 5);
    m.part_count = 4; /* 大きさと part_blocks からは 3 */
    put_meta(ID, &m);
    put_parts_data(ID, PB_SMALL, SZ);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_catalog_scan(&CAT));
    iso_index_job_t job;
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_index_job_init(&job, ID));
    expect_invalid(&job, ISO_INVALID_PART_MISMATCH);
}

void test_part_count_over_the_maximum_is_part_mismatch(void) {
    /* 17 個のパーツ(PB_SMALL のパーツが 17 個必要な大きさ)。meta の検査は part_count 16 まで */
    iso_meta_t m = mk_meta("img", 17 * PART_SMALL, PB_SMALL, ISO_META_READY, 5);
    m.part_count = 16;
    put_meta(ID, &m);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_catalog_scan(&CAT));
    iso_index_job_t job;
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_index_job_init(&job, ID));
    expect_invalid(&job, ISO_INVALID_PART_MISMATCH);
}

/* ---- I/O エラー、対象の状態 -------------------------------------------------------------------- */

static void expect_ioerr(iso_index_job_t *job, int err) {
    iso_step_result_t r = run(job, NULL);
    TEST_ASSERT_EQUAL_INT(ISO_STEP_DONE, r.kind);
    TEST_ASSERT_EQUAL_INT(ISO_OUTCOME_IOERR, job->outcome);
    TEST_ASSERT_EQUAL_INT(err, job->err);
    TEST_ASSERT_NULL(job->table);
    TEST_ASSERT_NULL(job->pool);
    TEST_ASSERT_EQUAL_INT(0, ALLOC_LIVE);
}

void test_stat_io_error(void) {
    make_ready();
    mock_fs_fail_path(&FS, "/iso/" ID "/p001");
    iso_index_job_t job;
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_index_job_init(&job, ID));
    expect_ioerr(&job, ISO_ERR_IO);
    mock_fs_fail_path(&FS, NULL);
}

void test_get_extents_io_error(void) {
    make_ready();
    TEST_ASSERT_TRUE(mock_fs_fail_extents(&FS, "/iso/" ID "/p002", ISO_ERR_IO));
    iso_index_job_t job;
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_index_job_init(&job, ID));
    expect_ioerr(&job, ISO_ERR_IO);
}

void test_allocation_failures(void) {
    make_ready();
    ALLOC_CALLS = 0;
    ALLOC_FAIL_AT = 1; /* 作業領域 */
    iso_index_job_t job;
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_index_job_init(&job, ID));
    expect_ioerr(&job, ISO_ERR_NOMEM);
    ALLOC_CALLS = 0;
    ALLOC_FAIL_AT = 2; /* 表 */
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_index_job_init(&job, ID));
    expect_ioerr(&job, ISO_ERR_NOMEM);
    ALLOC_FAIL_AT = 0;
}

void test_unknown_or_unusable_image_is_an_error_outcome(void) {
    iso_index_job_t job;
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_index_job_init(&job, "0000000b"));
    expect_ioerr(&job, ISO_ERR_NOT_FOUND);
    /* incomplete のイメージには作らない */
    iso_meta_t m = mk_meta("img", SZ, PB_SMALL, ISO_META_INCOMPLETE, 5);
    put_meta(ID, &m);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_catalog_scan(&CAT));
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_index_job_init(&job, ID));
    expect_ioerr(&job, ISO_ERR_NOT_FOUND);
    /* meta_corrupt */
    TEST_ASSERT_EQUAL_INT(ISO_OK, mock_fs_add_dir(&FS, "/iso/0000000c"));
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_catalog_scan(&CAT));
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_index_job_init(&job, "0000000c"));
    expect_ioerr(&job, ISO_ERR_NOT_FOUND);
}

void test_job_init_rejects_a_bad_id(void) {
    iso_index_job_t job;
    TEST_ASSERT_EQUAL_INT(ISO_ERR_INVALID, iso_index_job_init(&job, "ABCDEF01"));
    TEST_ASSERT_EQUAL_INT(ISO_ERR_INVALID, iso_index_job_init(&job, "123"));
    TEST_ASSERT_EQUAL_INT(ISO_ERR_INVALID, iso_index_job_init(NULL, ID));
    iso_finish_job_t fj;
    TEST_ASSERT_EQUAL_INT(ISO_ERR_INVALID, iso_finish_job_init(&fj, "xx"));
}

void test_release_in_the_middle_frees_the_work_area(void) {
    make_ready();
    iso_index_job_t job;
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_index_job_init(&job, ID));
    iso_index_step(&UP, &job);
    iso_index_step(&UP, &job);
    TEST_ASSERT_EQUAL_INT(1, ALLOC_LIVE);
    iso_index_job_release(&UP, &job);
    TEST_ASSERT_EQUAL_INT(0, ALLOC_LIVE);
}

void test_extent_pool_is_shared_across_parts(void) {
    /* 各パーツ 1 本(3 本)。pool_used が積み上がり、inputs の各配列が重ならない */
    make_ready();
    set_known_extents();
    iso_index_job_t job;
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_index_job_init(&job, ID));
    iso_index_step(&UP, &job);
    iso_index_step(&UP, &job);
    iso_index_step(&UP, &job);
    TEST_ASSERT_EQUAL_UINT(2, job.pool_used);
    TEST_ASSERT_EQUAL_UINT64(34816, job.inputs[0].file_bytes);
    TEST_ASSERT_EQUAL_UINT64(34816, job.inputs[1].file_bytes);
    TEST_ASSERT_TRUE(job.inputs[1].extents == job.inputs[0].extents + 1);
    iso_index_job_release(&UP, &job);
}


void test_exactly_16_parts_are_accepted(void) {
    iso_meta_t m = mk_meta("img", 16 * PART_SMALL, PB_SMALL, ISO_META_READY, 5);
    TEST_ASSERT_EQUAL_UINT16(16, m.part_count);
    put_meta(ID, &m);
    put_parts_data(ID, PB_SMALL, 16 * PART_SMALL);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_catalog_scan(&CAT));
    iso_index_job_t job;
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_index_job_init(&job, ID));
    int steps = 0;
    iso_step_result_t r = run(&job, &steps);
    TEST_ASSERT_EQUAL_INT(ISO_STEP_DONE, r.kind);
    TEST_ASSERT_EQUAL_INT(ISO_OUTCOME_OK, job.outcome);
    TEST_ASSERT_EQUAL_INT(1 + 16 + 1, steps);
    TEST_ASSERT_EQUAL_UINT32(16, iso_extent_table_part_count(job.table));
    iso_index_job_release(&UP, &job);
}

void test_job_init_clears_the_whole_job(void) {
    iso_index_job_t job;
    memset(&job, 0xAA, sizeof(job));
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_index_job_init(&job, ID));
    TEST_ASSERT_NULL(job.pool);
    TEST_ASSERT_NULL(job.table);
    TEST_ASSERT_EQUAL_UINT(0, job.pool_used);
    TEST_ASSERT_EQUAL_UINT32(0, job.part);
    TEST_ASSERT_EQUAL_INT(ISO_OUTCOME_NONE, job.outcome);
    TEST_ASSERT_EQUAL_STRING(ID, job.id);
    iso_finish_job_t fj;
    memset(&fj, 0xAA, sizeof(fj));
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_finish_job_init(&fj, ID));
    TEST_ASSERT_EQUAL_UINT8(0, fj.phase);
    TEST_ASSERT_FALSE(fj.hash_started);
    TEST_ASSERT_FALSE(fj.have_digest);
    TEST_ASSERT_EQUAL_INT(ISO_OUTCOME_NONE, fj.fail);
    TEST_ASSERT_NULL(fj.idx.pool);
    TEST_ASSERT_EQUAL_STRING(ID, fj.idx.id);
}

void test_step_after_an_invalid_or_error_outcome_stays_done_and_allocates_nothing(void) {
    static uint8_t patch_buf[34816];
    gen_fill(patch_buf, 34816, sizeof(patch_buf));
    make_ready();
    TEST_ASSERT_EQUAL_INT(ISO_OK, IFS.remove(&FS, "/iso/" ID "/p001"));
    iso_index_job_t job;
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_index_job_init(&job, ID));
    iso_step_result_t r = run(&job, NULL);
    TEST_ASSERT_EQUAL_INT(ISO_OUTCOME_INVALID, job.outcome);
    ALLOC_CALLS = 0;
    FS.n_stat = 0;
    r = iso_index_step(&UP, &job); /* invalid で終わった後は、何もしない */
    TEST_ASSERT_EQUAL_INT(ISO_STEP_DONE, r.kind);
    TEST_ASSERT_EQUAL_INT(0, ALLOC_CALLS);
    TEST_ASSERT_EQUAL_INT(0, FS.n_stat);
    TEST_ASSERT_EQUAL_INT(ISO_OUTCOME_INVALID, job.outcome);
    /* I/O エラーで終わった後も */
    TEST_ASSERT_TRUE(mock_fs_fail_extents(&FS, "/iso/" ID "/p002", ISO_ERR_IO));
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_index_job_init(&job, ID));
    TEST_ASSERT_EQUAL_INT(ISO_OK, mock_fs_add_file(&FS, "/iso/" ID "/p001", patch_buf, 34816, 0));
    r = run(&job, NULL);
    TEST_ASSERT_EQUAL_INT(ISO_OUTCOME_IOERR, job.outcome);
    ALLOC_CALLS = 0;
    FS.n_stat = 0;
    r = iso_index_step(&UP, &job);
    TEST_ASSERT_EQUAL_INT(ISO_STEP_DONE, r.kind);
    TEST_ASSERT_EQUAL_INT(0, ALLOC_CALLS);
    TEST_ASSERT_EQUAL_INT(0, FS.n_stat);
    TEST_ASSERT_EQUAL_INT(ISO_OUTCOME_IOERR, job.outcome);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_index_builds_the_table_part_by_part);
    RUN_TEST(test_index_step_count_is_init_plus_parts_plus_build);
    RUN_TEST(test_index_after_done_stays_done);
    RUN_TEST(test_index_clips_orphan_tail_extents);
    RUN_TEST(test_index_without_blockdev_uses_epoch_0);
    RUN_TEST(test_epoch_is_recorded_at_the_start);
    RUN_TEST(test_missing_part_is_part_mismatch);
    RUN_TEST(test_missing_first_part_is_part_mismatch);
    RUN_TEST(test_wrong_size_part_is_part_mismatch);
    RUN_TEST(test_part_that_is_a_directory_is_part_mismatch);
    RUN_TEST(test_extents_that_do_not_cover_the_file_are_part_mismatch);
    RUN_TEST(test_overflowing_extent_is_part_mismatch);
    RUN_TEST(test_get_extents_not_found_is_part_mismatch);
    RUN_TEST(test_too_fragmented_from_the_fs);
    RUN_TEST(test_part_count_that_disagrees_with_the_meta_is_part_mismatch);
    RUN_TEST(test_part_count_over_the_maximum_is_part_mismatch);
    RUN_TEST(test_stat_io_error);
    RUN_TEST(test_get_extents_io_error);
    RUN_TEST(test_allocation_failures);
    RUN_TEST(test_unknown_or_unusable_image_is_an_error_outcome);
    RUN_TEST(test_job_init_rejects_a_bad_id);
    RUN_TEST(test_release_in_the_middle_frees_the_work_area);
    RUN_TEST(test_extent_pool_is_shared_across_parts);
    RUN_TEST(test_exactly_16_parts_are_accepted);
    RUN_TEST(test_job_init_clears_the_whole_job);
    RUN_TEST(test_step_after_an_invalid_or_error_outcome_stays_done_and_allocates_nothing);
    return UNITY_END();
}
