/*
 * iso_upload_begin: 検査の順序(WEB_API 8.2)、枠、空き容量の式(設計 4.2)、失敗の巻き戻し。期待値は手書き。
 */
#include "up_common.h"

void setUp(void) { fx_up_setup(0, 0); }
void tearDown(void) { fx_up_teardown(); }

#define SZ 100000ull /* 3 パーツ(PB_SMALL)ではなく、既定のパーツ 1 つ */

static int begin(const char *name, uint64_t size) {
    char id[ISO_ID_LEN + 1];
    return iso_upload_begin(&UP, name, strlen(name), size, NULL, id);
}

void test_begin_creates_the_image_and_takes_the_slot(void) {
    fx_settle();
    char id[ISO_ID_LEN + 1];
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_upload_begin(&UP, "ubuntu", 6, SZ, NULL, id));
    TEST_ASSERT_EQUAL_STRING("11110001", id);
    TEST_ASSERT_TRUE(mock_fs_log_is(&FS, 0, "mkdir /iso/11110001"));
    TEST_ASSERT_TRUE(mock_fs_log_is(&FS, 1, "overwrite /iso/11110001/meta.a len=256"));
    TEST_ASSERT_EQUAL_INT(2, FS.nlog);
    iso_catalog_entry_t *e = iso_catalog_find(&CAT, id);
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_INT(ISO_STATE_UPLOADING, e->state);
    TEST_ASSERT_EQUAL_UINT64(SZ, e->meta.size_bytes);
    TEST_ASSERT_EQUAL_UINT64(0, e->received);
    TEST_ASSERT_EQUAL_UINT8(ISO_SHA_NONE, e->meta.sha_flag);
    TEST_ASSERT_TRUE(iso_upload_slot_busy(&UP));
    TEST_ASSERT_EQUAL_STRING("11110001", iso_upload_slot_id(&UP));
    TEST_ASSERT_EQUAL_INT(1, VIEW_CALLS);
    TEST_ASSERT_EQUAL_INT(0, FS.open_files); /* p000 は最初の PUT で作る */
    TEST_ASSERT_FALSE(mock_fs_exists(&FS, "/iso/11110001/p000"));
}

void test_begin_with_a_declared_sha_is_stored_as_flag_1(void) {
    uint8_t sha[32];
    hex32("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", sha);
    char id[ISO_ID_LEN + 1];
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_upload_begin(&UP, "a", 1, SZ, sha, id));
    iso_catalog_entry_t *e = iso_catalog_find(&CAT, id);
    TEST_ASSERT_EQUAL_UINT8(ISO_SHA_DECLARED, e->meta.sha_flag);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(sha, e->meta.sha256, 32);
}

void test_begin_size_boundaries(void) {
    char id[ISO_ID_LEN + 1];
    TEST_ASSERT_EQUAL_INT(ISO_ERR_INVALID, iso_upload_begin(&UP, "a", 1, 34815, NULL, id));
    TEST_ASSERT_FALSE(iso_upload_slot_busy(&UP));
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_upload_begin(&UP, "a", 1, 34816, NULL, id));
    iso_upload_abort(&UP, id);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_upload_begin(&UP, "b", 1, 16 * GIB, NULL, id));
    TEST_ASSERT_EQUAL_UINT64(16 * GIB, iso_catalog_find(&CAT, id)->meta.size_bytes);
    TEST_ASSERT_EQUAL_UINT16(16, iso_catalog_find(&CAT, id)->meta.part_count);
    iso_upload_abort(&UP, id);
    TEST_ASSERT_EQUAL_INT(ISO_ERR_INVALID, iso_upload_begin(&UP, "c", 1, 16 * GIB + 1, NULL, id));
    TEST_ASSERT_EQUAL_INT(ISO_ERR_INVALID, iso_upload_begin(&UP, "c", 1, 0, NULL, id));
    TEST_ASSERT_EQUAL_INT(ISO_ERR_INVALID, iso_upload_begin(&UP, "c", 1, UINT64_MAX, NULL, id));
}

void test_begin_rejects_bad_names_and_arguments(void) {
    char id[ISO_ID_LEN + 1];
    TEST_ASSERT_EQUAL_INT(ISO_ERR_INVALID, iso_upload_begin(&UP, "", 0, SZ, NULL, id));
    TEST_ASSERT_EQUAL_INT(ISO_ERR_INVALID, iso_upload_begin(&UP, "a/b", 3, SZ, NULL, id));
    TEST_ASSERT_EQUAL_INT(ISO_ERR_INVALID, iso_upload_begin(&UP, " a", 2, SZ, NULL, id));
    char big[65];
    memset(big, 'x', sizeof(big));
    TEST_ASSERT_EQUAL_INT(ISO_ERR_INVALID, iso_upload_begin(&UP, big, 64, SZ, NULL, id));
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_upload_begin(&UP, big, 63, SZ, NULL, id));
    iso_upload_abort(&UP, id);
    TEST_ASSERT_EQUAL_INT(ISO_ERR_INVALID, iso_upload_begin(&UP, NULL, 1, SZ, NULL, id));
    TEST_ASSERT_EQUAL_INT(ISO_ERR_INVALID, iso_upload_begin(&UP, "a", 1, SZ, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(ISO_ERR_INVALID, iso_upload_begin(NULL, "a", 1, SZ, NULL, id));
}

void test_begin_order_slot_then_name_then_count_then_storage(void) {
    char id[ISO_ID_LEN + 1];
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_upload_begin(&UP, "one", 3, SZ, NULL, id));
    /* 枠が使用中: 同名でも upload_in_progress */
    TEST_ASSERT_EQUAL_INT(ISO_RES_UPLOAD_IN_PROGRESS, begin("one", SZ));
    TEST_ASSERT_EQUAL_INT(ISO_RES_UPLOAD_IN_PROGRESS, begin("two", SZ));
    /* 名前や大きさの不正(400)は、枠より先 */
    TEST_ASSERT_EQUAL_INT(ISO_ERR_INVALID, begin("", SZ));
    TEST_ASSERT_EQUAL_INT(ISO_ERR_INVALID, begin("two", 1));
    /* 枠を空ける(incomplete)と、同名が name_exists */
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_upload_idle_close(&UP, id));
    TEST_ASSERT_FALSE(iso_upload_slot_busy(&UP));
    TEST_ASSERT_EQUAL_INT(ISO_RES_NAME_EXISTS, begin("one", SZ));
    /* 上限(16 個): 同名は name_exists が先、別名は too_many_images が空き不足より先 */
    for (int i = 1; i < 16; i++) {
        char n[8];
        snprintf(n, sizeof(n), "i%02d", i);
        TEST_ASSERT_EQUAL_INT(ISO_OK, iso_upload_begin(&UP, n, strlen(n), SZ, NULL, id));
        TEST_ASSERT_EQUAL_INT(ISO_OK, iso_upload_idle_close(&UP, id));
    }
    TEST_ASSERT_EQUAL_UINT(16, CAT.count);
    iso_upload_set_free(&UP, 64 * GIB, 0); /* 空きなしでも */
    TEST_ASSERT_EQUAL_INT(ISO_RES_NAME_EXISTS, begin("one", SZ));
    TEST_ASSERT_EQUAL_INT(ISO_RES_TOO_MANY_IMAGES, begin("new", SZ));
}

/* ---- 空き容量: 必要 = size + 64 MiB(ISO_FREE_RESERVE_MIB) ---------------------------------------- */

void test_storage_boundary_is_size_plus_64_mib(void) {
    const uint64_t size = 5 * GIB;
    iso_upload_set_free(&UP, 64 * GIB, size + 64 * MIB - 1);
    TEST_ASSERT_EQUAL_INT(ISO_RES_INSUFFICIENT_STORAGE, begin("a", size));
    TEST_ASSERT_FALSE(iso_upload_slot_busy(&UP));
    TEST_ASSERT_EQUAL_UINT(0, CAT.count);
    iso_upload_set_free(&UP, 64 * GIB, size + 64 * MIB);
    TEST_ASSERT_EQUAL_INT(ISO_OK, begin("a", size));
}

void test_size_over_total_bytes_is_507_even_with_enough_free(void) {
    iso_upload_set_free(&UP, 2 * GIB, 60 * GIB); /* free > total は、キャッシュの採取の食い違いの想定 */
    TEST_ASSERT_EQUAL_INT(ISO_RES_INSUFFICIENT_STORAGE, begin("a", 2 * GIB + 1));
    iso_upload_set_free(&UP, 2 * GIB, 60 * GIB);
    TEST_ASSERT_EQUAL_INT(ISO_OK, begin("a", 2 * GIB));
}

void test_storage_unknown_cache_rejects(void) {
    /* キャッシュを一度も採取していない(マウント前): 空きなしとして拒否する */
    iso_upload_t u2;
    iso_upload_cfg_t cfg;
    fx_up_cfg(&cfg, 0, 0);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_upload_init(&u2, &cfg));
    char id[9];
    TEST_ASSERT_EQUAL_INT(ISO_RES_INSUFFICIENT_STORAGE, iso_upload_begin(&u2, "a", 1, SZ, NULL, id));
    TEST_ASSERT_EQUAL_UINT64(0, iso_upload_free_estimate(&u2));
}

void test_storage_formula_design_example(void) {
    /* 設計 4.2: 10 GiB で採取し、6 GiB のイメージを 5 GiB まで受信して中断: 10 - 5 - (6 - 5) = 4 GiB */
    iso_upload_set_free(&UP, 64 * GIB, 10 * GIB);
    iso_meta_t m = mk_meta("big", 6 * GIB, 0, ISO_META_INCOMPLETE, 1);
    put_meta("00000009", &m);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_catalog_scan(&CAT));
    iso_catalog_entry_t *e = iso_catalog_find(&CAT, "00000009");
    TEST_ASSERT_NOT_NULL(e);
    e->received = 5 * GIB;
    UP.consumed_since = 5 * GIB; /* 採取の後に 5 GiB 書いた */
    TEST_ASSERT_EQUAL_UINT64(4 * GIB, iso_upload_free_estimate(&UP));
    TEST_ASSERT_EQUAL_INT(ISO_RES_INSUFFICIENT_STORAGE, begin("x", 4 * GIB - 64 * MIB + 1));
    TEST_ASSERT_EQUAL_INT(ISO_OK, begin("x", 4 * GIB - 64 * MIB));
}

void test_storage_counts_uploading_and_incomplete_but_not_ready_or_invalid(void) {
    iso_upload_set_free(&UP, 64 * GIB, 10 * GIB);
    iso_meta_t a = mk_meta("a", 3 * GIB, 0, ISO_META_INCOMPLETE, 1);
    iso_meta_t b = mk_meta("b", 2 * GIB, 0, ISO_META_READY, 1);
    iso_meta_t c = mk_meta("c", 1 * GIB, 0, ISO_META_INVALID, 1);
    c.invalid_reason = ISO_INVALID_NOT_ISO;
    put_meta("0000000a", &a);
    put_meta("0000000b", &b);
    put_meta("0000000c", &c);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_catalog_scan(&CAT));
    /* a は received 0(パーツなし)= 3 GiB を予約。ready と invalid は数えない */
    TEST_ASSERT_EQUAL_UINT64(7 * GIB, iso_upload_free_estimate(&UP));
}

void test_free_estimate_saturates_at_zero(void) {
    iso_upload_set_free(&UP, 64 * GIB, 1 * GIB);
    UP.consumed_since = 5 * GIB;
    TEST_ASSERT_EQUAL_UINT64(0, iso_upload_free_estimate(&UP));
}

void test_set_free_resets_consumed_since(void) {
    UP.consumed_since = 12345;
    iso_upload_set_free(&UP, 8 * GIB, 7 * GIB);
    TEST_ASSERT_EQUAL_UINT64(0, UP.consumed_since);
    TEST_ASSERT_EQUAL_UINT64(7 * GIB, iso_upload_free_estimate(&UP));
}

void test_refresh_free_reads_mount_info_and_keeps_the_cache_on_error(void) {
    mock_fs_set_mount_info(&FS, 32 * GIB, 20 * GIB, 32768);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_upload_refresh_free(&UP));
    TEST_ASSERT_EQUAL_UINT64(20 * GIB, iso_upload_free_estimate(&UP));
    TEST_ASSERT_EQUAL_UINT64(32 * GIB, UP.total_bytes);
    mock_fs_set_mount_info(&FS, 1 * GIB, 1 * GIB, 32768);
    mock_fs_fail_mount_info(&FS, ISO_ERR_IO);
    TEST_ASSERT_EQUAL_INT(ISO_ERR_IO, iso_upload_refresh_free(&UP));
    TEST_ASSERT_EQUAL_UINT64(20 * GIB, iso_upload_free_estimate(&UP)); /* 変わらない */
}

/* ---- 失敗の巻き戻し ---------------------------------------------------------------------- */

void test_begin_mkdir_failure_leaves_nothing(void) {
    fx_settle();
    mock_fs_error_at(&FS, 1, ISO_ERR_IO);
    TEST_ASSERT_EQUAL_INT(ISO_ERR_IO, begin("a", SZ));
    TEST_ASSERT_FALSE(iso_upload_slot_busy(&UP));
    TEST_ASSERT_EQUAL_UINT(0, CAT.count);
    TEST_ASSERT_EQUAL_INT(0, VIEW_CALLS);
}

void test_begin_meta_failure_cleans_up_and_keeps_the_slot_free(void) {
    fx_settle();
    mock_fs_error_at(&FS, 2, ISO_ERR_IO); /* mkdir の次 = meta.a の書き込み */
    TEST_ASSERT_EQUAL_INT(ISO_ERR_IO, begin("a", SZ));
    TEST_ASSERT_FALSE(iso_upload_slot_busy(&UP));
    TEST_ASSERT_EQUAL_UINT(0, CAT.count);
    TEST_ASSERT_FALSE(mock_fs_exists(&FS, "/iso/11110001"));
    TEST_ASSERT_EQUAL_INT(0, VIEW_CALLS);
    mock_fs_reset_counter(&FS);
    TEST_ASSERT_EQUAL_INT(ISO_OK, begin("a", SZ)); /* 続けて始められる */
}

void test_begin_starts_sha_state_and_stamps_the_time(void) {
    NOW_MS = 424242;
    char id[9];
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_upload_begin(&UP, "a", 1, SZ, NULL, id));
    TEST_ASSERT_TRUE(UP.sha_valid);
    TEST_ASSERT_EQUAL_STRING(id, UP.sha_id);
    TEST_ASSERT_EQUAL_UINT64(0, UP.sha_bytes);
    TEST_ASSERT_EQUAL_UINT64(424242, UP.last_put_ms);
}

void test_init_validates_the_config(void) {
    iso_upload_t u;
    iso_upload_cfg_t cfg;
    fx_up_cfg(&cfg, 0, 0);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_upload_init(&u, &cfg));
    TEST_ASSERT_EQUAL_UINT32(131072, iso_upload_chunk_bytes(&u)); /* 既定 128 KiB */
    TEST_ASSERT_EQUAL_UINT32(4194304, u.cfg.sync_bytes);           /* 既定 4 MiB */
    iso_upload_cfg_t bad = cfg;
    bad.cat = NULL;
    TEST_ASSERT_EQUAL_INT(ISO_ERR_INVALID, iso_upload_init(&u, &bad));
    bad = cfg;
    bad.io_buf_size = 0;
    TEST_ASSERT_EQUAL_INT(ISO_ERR_INVALID, iso_upload_init(&u, &bad));
    bad = cfg;
    bad.sha_state = NULL;
    TEST_ASSERT_EQUAL_INT(ISO_ERR_INVALID, iso_upload_init(&u, &bad));
    bad = cfg;
    bad.alloc = NULL;
    TEST_ASSERT_EQUAL_INT(ISO_ERR_INVALID, iso_upload_init(&u, &bad));
    bad = cfg;
    bad.clock = NULL;
    TEST_ASSERT_EQUAL_INT(ISO_ERR_INVALID, iso_upload_init(&u, &bad));
    TEST_ASSERT_EQUAL_INT(ISO_ERR_INVALID, iso_upload_init(NULL, &cfg));
    TEST_ASSERT_EQUAL_INT(ISO_ERR_INVALID, iso_upload_init(&u, NULL));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_begin_creates_the_image_and_takes_the_slot);
    RUN_TEST(test_begin_with_a_declared_sha_is_stored_as_flag_1);
    RUN_TEST(test_begin_size_boundaries);
    RUN_TEST(test_begin_rejects_bad_names_and_arguments);
    RUN_TEST(test_begin_order_slot_then_name_then_count_then_storage);
    RUN_TEST(test_storage_boundary_is_size_plus_64_mib);
    RUN_TEST(test_size_over_total_bytes_is_507_even_with_enough_free);
    RUN_TEST(test_storage_unknown_cache_rejects);
    RUN_TEST(test_storage_formula_design_example);
    RUN_TEST(test_storage_counts_uploading_and_incomplete_but_not_ready_or_invalid);
    RUN_TEST(test_free_estimate_saturates_at_zero);
    RUN_TEST(test_set_free_resets_consumed_since);
    RUN_TEST(test_refresh_free_reads_mount_info_and_keeps_the_cache_on_error);
    RUN_TEST(test_begin_mkdir_failure_leaves_nothing);
    RUN_TEST(test_begin_meta_failure_cleans_up_and_keeps_the_slot_free);
    RUN_TEST(test_begin_starts_sha_state_and_stamps_the_time);
    RUN_TEST(test_init_validates_the_config);
    return UNITY_END();
}
