/*
 * iso_catalog: 起動時のスキャン(設計 3.3 末尾、3.4)と received の計算。期待値は手書き。
 */
#include "ic_common.h"

void setUp(void) { fx_setup(); }
void tearDown(void) { fx_teardown(); }

#define MIB 1048576ull

/* 大きさ 2.5 GiB = 3 パーツ(1 GiB、1 GiB、0.5 GiB) */
#define SIZE_25G (2 * GIB + 512 * MIB)

void test_scan_empty_card_creates_directories(void) {
    mock_fs_free(&FS);
    mock_fs_init(&FS);
    IFS = mock_fs_iface(&FS);
    iso_catalog_init(&CAT, &IFS, &IRND);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_catalog_scan(&CAT));
    TEST_ASSERT_EQUAL_UINT(0, CAT.count);
    TEST_ASSERT_EQUAL_UINT32(0, CAT.hidden);
    TEST_ASSERT_TRUE(mock_fs_is_dir(&FS, "/iso"));
    TEST_ASSERT_TRUE(mock_fs_is_dir(&FS, "/iso/import"));
    /* 2 回目: すでにあっても成功 */
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_catalog_scan(&CAT));
    TEST_ASSERT_EQUAL_UINT(0, CAT.count);
}

void test_scan_ignores_directories_that_are_not_ids(void) {
    TEST_ASSERT_EQUAL_INT(ISO_OK, mock_fs_add_dir(&FS, "/iso/A1B2C3D4"));   /* 大文字 */
    TEST_ASSERT_EQUAL_INT(ISO_OK, mock_fs_add_dir(&FS, "/iso/abc"));        /* 短い */
    TEST_ASSERT_EQUAL_INT(ISO_OK, mock_fs_add_dir(&FS, "/iso/a1b2c3d4x"));  /* 長い */
    TEST_ASSERT_EQUAL_INT(ISO_OK, mock_fs_add_dir(&FS, "/iso/zzzzzzzz"));   /* 16 進でない */
    TEST_ASSERT_EQUAL_INT(ISO_OK, mock_fs_add_file(&FS, "/iso/cafebabe", "x", 1, 0)); /* 形式は id だがファイル */
    iso_meta_t m = mk_meta("Alpha", 100000, ISO_META_READY, 1);
    put_slot("deadbeef", 0, &m);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_catalog_scan(&CAT));
    TEST_ASSERT_EQUAL_UINT(1, CAT.count);
    TEST_ASSERT_EQUAL_UINT32(0, CAT.hidden); /* 無視したものは hidden に数えない */
    TEST_ASSERT_EQUAL_STRING("deadbeef", CAT.images[0].id);
}

void test_scan_state_mapping_per_image(void) {
    iso_meta_t up = mk_meta("Up", SIZE_25G, ISO_META_UPLOADING, 3);
    put_slot("00000001", 0, &up);
    put_part("00000001", 0, GIB);
    put_part("00000001", 1, 600 * MIB);

    iso_meta_t inc = mk_meta("Inc", SIZE_25G, ISO_META_INCOMPLETE, 4);
    put_slot("00000002", 1, &inc);
    put_part("00000002", 0, 300 * MIB);

    iso_meta_t rd = mk_meta("Rd", 5000000, ISO_META_READY, 9);
    rd.sha_flag = ISO_SHA_COMPUTED;
    memset(rd.sha256, 0xAB, 32);
    put_slot("00000003", 0, &rd);
    /* ready は p000 が無くても、received = size_bytes (INDEX が part_mismatch にする) */

    iso_meta_t inv = mk_meta("Inv", 5000000, ISO_META_INVALID, 6);
    inv.invalid_reason = ISO_INVALID_PART_MISMATCH;
    put_slot("00000004", 0, &inv);
    put_part("00000004", 0, 4096);

    iso_meta_t del = mk_meta("Del", 5000000, ISO_META_DELETING, 2);
    put_slot("00000005", 0, &del);
    put_part("00000005", 0, 5000000);

    TEST_ASSERT_EQUAL_INT(ISO_OK, mock_fs_add_dir(&FS, "/iso/00000006")); /* meta なし */
    iso_meta_t torn = mk_meta("Torn", 5000000, ISO_META_READY, 1);
    put_slot("00000007", 0, &torn);
    TEST_ASSERT_TRUE(mock_fs_set_size(&FS, "/iso/00000007/meta.a", 128)); /* 書き込み途中で電源断 */

    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_catalog_scan(&CAT));
    TEST_ASSERT_EQUAL_UINT(7, CAT.count);
    TEST_ASSERT_EQUAL_UINT32(0, CAT.hidden);

    iso_catalog_entry_t *e = iso_catalog_find(&CAT, "00000001");
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_INT(ISO_STATE_INCOMPLETE, e->state); /* uploading -> incomplete */
    TEST_ASSERT_EQUAL_UINT8(ISO_META_UPLOADING, e->meta.state); /* meta はそのまま(書き換えない) */
    TEST_ASSERT_EQUAL_UINT64(GIB + 600 * MIB, e->received);
    TEST_ASSERT_TRUE(e->meta_valid);
    TEST_ASSERT_FALSE(e->resume_delete);
    TEST_ASSERT_EQUAL_INT8(0, e->adopted);
    TEST_ASSERT_EQUAL_STRING("Up", e->meta.name);

    e = iso_catalog_find(&CAT, "00000002");
    TEST_ASSERT_EQUAL_INT(ISO_STATE_INCOMPLETE, e->state);
    TEST_ASSERT_EQUAL_UINT64(300 * MIB, e->received); /* 満たないパーツで止める */
    TEST_ASSERT_EQUAL_INT8(1, e->adopted);

    e = iso_catalog_find(&CAT, "00000003");
    TEST_ASSERT_EQUAL_INT(ISO_STATE_READY, e->state);
    TEST_ASSERT_EQUAL_UINT64(5000000, e->received);
    TEST_ASSERT_EQUAL_UINT8(ISO_SHA_COMPUTED, e->meta.sha_flag);
    TEST_ASSERT_EQUAL_HEX8(0xAB, e->meta.sha256[31]);

    e = iso_catalog_find(&CAT, "00000004");
    TEST_ASSERT_EQUAL_INT(ISO_STATE_INVALID, e->state);
    TEST_ASSERT_EQUAL_UINT8(ISO_INVALID_PART_MISMATCH, e->invalid_reason);
    TEST_ASSERT_TRUE(e->meta_valid);
    TEST_ASSERT_EQUAL_UINT64(4096, e->received);

    e = iso_catalog_find(&CAT, "00000005");
    TEST_ASSERT_EQUAL_INT(ISO_STATE_DELETING, e->state);
    TEST_ASSERT_TRUE(e->resume_delete); /* 「削除を続ける」印 */
    TEST_ASSERT_EQUAL_UINT64(0, e->received);

    e = iso_catalog_find(&CAT, "00000006");
    TEST_ASSERT_EQUAL_INT(ISO_STATE_INVALID, e->state);
    TEST_ASSERT_EQUAL_UINT8(ISO_INVALID_META_CORRUPT, e->invalid_reason);
    TEST_ASSERT_FALSE(e->meta_valid);
    TEST_ASSERT_FALSE(e->resume_delete);
    TEST_ASSERT_EQUAL_INT8(-1, e->adopted);
    TEST_ASSERT_EQUAL_UINT64(0, e->received);

    e = iso_catalog_find(&CAT, "00000007");
    TEST_ASSERT_EQUAL_INT(ISO_STATE_INVALID, e->state);
    TEST_ASSERT_EQUAL_UINT8(ISO_INVALID_META_CORRUPT, e->invalid_reason);
    TEST_ASSERT_FALSE(e->meta_valid);
}

void test_scan_resume_delete_only_for_deleting(void) {
    iso_meta_t states[5];
    for (int i = 0; i < 5; i++) states[i] = mk_meta("X", 100000, (uint8_t)i, 1);
    states[3].invalid_reason = 1;
    const char *ids[5] = {"00000010", "00000011", "00000012", "00000013", "00000014"};
    for (int i = 0; i < 5; i++) put_slot(ids[i], 0, &states[i]);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_catalog_scan(&CAT));
    TEST_ASSERT_EQUAL_UINT(5, CAT.count);
    for (int i = 0; i < 5; i++) {
        iso_catalog_entry_t *e = iso_catalog_find(&CAT, ids[i]);
        TEST_ASSERT_NOT_NULL(e);
        TEST_ASSERT_EQUAL_INT_MESSAGE(i == 4 ? 1 : 0, e->resume_delete ? 1 : 0, ids[i]);
    }
    /* meta の状態 -> 実行時の状態 */
    TEST_ASSERT_EQUAL_INT(ISO_STATE_INCOMPLETE, iso_catalog_find(&CAT, "00000010")->state);
    TEST_ASSERT_EQUAL_INT(ISO_STATE_READY, iso_catalog_find(&CAT, "00000011")->state);
    TEST_ASSERT_EQUAL_INT(ISO_STATE_INCOMPLETE, iso_catalog_find(&CAT, "00000012")->state);
    TEST_ASSERT_EQUAL_INT(ISO_STATE_INVALID, iso_catalog_find(&CAT, "00000013")->state);
    TEST_ASSERT_EQUAL_UINT8(ISO_INVALID_CHECKSUM_MISMATCH, iso_catalog_find(&CAT, "00000013")->invalid_reason);
    TEST_ASSERT_EQUAL_INT(ISO_STATE_DELETING, iso_catalog_find(&CAT, "00000014")->state);
}

void test_scan_adopts_the_newer_slot(void) {
    iso_meta_t a = mk_meta("Old", 100000, ISO_META_UPLOADING, 5);
    iso_meta_t b = mk_meta("New", 100000, ISO_META_READY, 6);
    put_slot("a1b2c3d4", 0, &a);
    put_slot("a1b2c3d4", 1, &b);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_catalog_scan(&CAT));
    TEST_ASSERT_EQUAL_STRING("New", CAT.images[0].meta.name);
    TEST_ASSERT_EQUAL_INT8(1, CAT.images[0].adopted);
    TEST_ASSERT_EQUAL_UINT32(6, CAT.images[0].meta.generation);
    TEST_ASSERT_EQUAL_INT(ISO_STATE_READY, CAT.images[0].state);
}

void test_scan_limits_to_16_images_and_counts_hidden_in_directory_order(void) {
    char id[9];
    for (int i = 0; i < 18; i++) {
        snprintf(id, sizeof(id), "%08x", 0x100 + i);
        iso_meta_t m = mk_meta("N", 100000, ISO_META_READY, 1);
        m.name[0] = (char)('A' + i);
        put_slot(id, 0, &m);
    }
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_catalog_scan(&CAT));
    TEST_ASSERT_EQUAL_UINT(16, CAT.count);
    TEST_ASSERT_EQUAL_UINT32(2, CAT.hidden);
    /* 見えるのは、ディレクトリの順の先頭の 16 個 */
    TEST_ASSERT_EQUAL_STRING("00000100", CAT.images[0].id);
    TEST_ASSERT_EQUAL_STRING("0000010f", CAT.images[15].id);
    TEST_ASSERT_NULL(iso_catalog_find(&CAT, "00000110"));
    /* 見えているものを消して再スキャンすると、隠れていたものが見える(設計 3.3) */
    iso_delete_job_t job;
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_delete_job_init(&job, "00000100"));
    iso_step_result_t r;
    int guard = 0;
    do {
        r = iso_catalog_delete_step(&CAT, &job);
        TEST_ASSERT_TRUE(++guard < 20);
    } while (r.kind == ISO_STEP_CONTINUE);
    TEST_ASSERT_EQUAL_INT(ISO_STEP_DONE, r.kind);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_catalog_scan(&CAT));
    TEST_ASSERT_EQUAL_UINT(16, CAT.count);
    TEST_ASSERT_EQUAL_UINT32(1, CAT.hidden);
    TEST_ASSERT_NOT_NULL(iso_catalog_find(&CAT, "00000110"));
}

void test_scan_exactly_16_images_has_no_hidden(void) {
    char id[9];
    for (int i = 0; i < 16; i++) {
        snprintf(id, sizeof(id), "%08x", 0x200 + i);
        iso_meta_t m = mk_meta("N", 100000, ISO_META_READY, 1);
        put_slot(id, 0, &m);
    }
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_catalog_scan(&CAT));
    TEST_ASSERT_EQUAL_UINT(16, CAT.count);
    TEST_ASSERT_EQUAL_UINT32(0, CAT.hidden);
}

void test_scan_resets_previous_contents(void) {
    iso_meta_t m = mk_meta("A", 100000, ISO_META_READY, 1);
    put_slot("00000001", 0, &m);
    put_slot("00000002", 0, &m);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_catalog_scan(&CAT));
    TEST_ASSERT_EQUAL_UINT(2, CAT.count);
    TEST_ASSERT_EQUAL_INT(ISO_OK, IFS.remove(&FS, "/iso/00000002/meta.a"));
    TEST_ASSERT_EQUAL_INT(ISO_OK, IFS.remove(&FS, "/iso/00000002"));
    CAT.hidden = 9;
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_catalog_scan(&CAT));
    TEST_ASSERT_EQUAL_UINT(1, CAT.count);
    TEST_ASSERT_EQUAL_UINT32(0, CAT.hidden);
    TEST_ASSERT_NULL(iso_catalog_find(&CAT, "00000002"));
}

void test_scan_io_errors_are_propagated(void) {
    iso_meta_t m = mk_meta("A", 100000, ISO_META_READY, 1);
    put_slot("00000001", 0, &m);
    mock_fs_fail_path(&FS, "/iso/00000001/meta.a");
    TEST_ASSERT_EQUAL_INT(ISO_ERR_IO, iso_catalog_scan(&CAT));
    mock_fs_fail_path(&FS, NULL);
    mock_fs_fail_dir_next(&FS, "/iso");
    TEST_ASSERT_EQUAL_INT(ISO_ERR_IO, iso_catalog_scan(&CAT));
    mock_fs_fail_dir_next(&FS, NULL);
    mock_fs_fail_path(&FS, "/iso");
    TEST_ASSERT_EQUAL_INT(ISO_ERR_IO, iso_catalog_scan(&CAT));
    mock_fs_fail_path(&FS, NULL);
    /* incomplete の received で、パーツの stat が失敗 */
    iso_meta_t up = mk_meta("U", 100000, ISO_META_INCOMPLETE, 1);
    put_slot("00000002", 0, &up);
    mock_fs_fail_path(&FS, "/iso/00000002/p000");
    TEST_ASSERT_EQUAL_INT(ISO_ERR_IO, iso_catalog_scan(&CAT));
    mock_fs_fail_path(&FS, NULL);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_catalog_scan(&CAT));
    /* カードの電源が落ちている(mkdir も IO エラー) */
    mock_fs_cut_at(&FS, 1, MOCK_TEAR_NONE);
    (void)IFS.remove(&FS, "/iso/none"); /* 1 回目の書き込みで電源が落ちる */
    TEST_ASSERT_EQUAL_INT(ISO_ERR_IO, iso_catalog_scan(&CAT));
}

void test_find_returns_only_exact_ids(void) {
    iso_meta_t m = mk_meta("A", 100000, ISO_META_READY, 1);
    put_slot("00000100", 0, &m);
    put_slot("00000200", 0, &m);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_catalog_scan(&CAT));
    TEST_ASSERT_EQUAL_UINT(2, CAT.count);
    TEST_ASSERT_NULL(iso_catalog_find(&CAT, NULL));
    TEST_ASSERT_NULL(iso_catalog_find(&CAT, "00000000")); /* 先頭より小さい */
    TEST_ASSERT_NULL(iso_catalog_find(&CAT, "00000150")); /* 間 */
    TEST_ASSERT_NULL(iso_catalog_find(&CAT, "00000300")); /* 末尾より大きい */
    TEST_ASSERT_NULL(iso_catalog_find(&CAT, "0000010"));  /* 前方一致 */
    TEST_ASSERT_EQUAL_PTR(&CAT.images[0], iso_catalog_find(&CAT, "00000100"));
    TEST_ASSERT_EQUAL_PTR(&CAT.images[1], iso_catalog_find(&CAT, "00000200"));
    /* 最後のエントリも見つかる(3 つ目) */
    put_slot("00000300", 0, &m);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_catalog_scan(&CAT));
    TEST_ASSERT_EQUAL_PTR(&CAT.images[2], iso_catalog_find(&CAT, "00000300"));
}

void test_remove_entry_in_the_middle_and_at_the_end(void) {
    iso_meta_t m = mk_meta("A", 100000, ISO_META_READY, 1);
    put_slot("00000100", 0, &m);
    put_slot("00000200", 0, &m);
    put_slot("00000300", 0, &m);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_catalog_scan(&CAT));
    iso_catalog_remove(&CAT, "00000200"); /* 真ん中 */
    TEST_ASSERT_EQUAL_UINT(2, CAT.count);
    TEST_ASSERT_EQUAL_STRING("00000100", CAT.images[0].id);
    TEST_ASSERT_EQUAL_STRING("00000300", CAT.images[1].id);
    iso_catalog_remove(&CAT, "00000300"); /* 末尾 */
    TEST_ASSERT_EQUAL_UINT(1, CAT.count);
    TEST_ASSERT_EQUAL_STRING("00000100", CAT.images[0].id);
    iso_catalog_remove(&CAT, "00000999"); /* ないもの */
    TEST_ASSERT_EQUAL_UINT(1, CAT.count);
    iso_catalog_remove(&CAT, "00000100");
    TEST_ASSERT_EQUAL_UINT(0, CAT.count);
}

void test_ensure_dirs_reports_each_failure(void) {
    mock_fs_free(&FS);
    mock_fs_init(&FS);
    IFS = mock_fs_iface(&FS);
    iso_catalog_init(&CAT, &IFS, &IRND);
    mock_fs_error_at(&FS, 1, ISO_ERR_NO_SPACE); /* mkdir /iso だけが失敗する(次の mkdir は成功する) */
    TEST_ASSERT_EQUAL_INT(ISO_ERR_NO_SPACE, iso_catalog_ensure_dirs(&CAT));
    TEST_ASSERT_FALSE(mock_fs_exists(&FS, "/iso/import")); /* /iso が作れなければ、先へ進まない */
    mock_fs_error_at(&FS, 1, ISO_ERR_NO_SPACE);
    TEST_ASSERT_EQUAL_INT(ISO_ERR_NO_SPACE, iso_catalog_scan(&CAT)); /* スキャンも、作れなければ失敗 */
    mock_fs_error_at(&FS, 2, ISO_ERR_NO_SPACE); /* mkdir /iso は成功し、/iso/import が失敗する */
    TEST_ASSERT_EQUAL_INT(ISO_ERR_NO_SPACE, iso_catalog_ensure_dirs(&CAT));
    TEST_ASSERT_TRUE(mock_fs_is_dir(&FS, "/iso"));
    mock_fs_error_at(&FS, 0, 0);
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_catalog_ensure_dirs(&CAT));
    TEST_ASSERT_TRUE(mock_fs_is_dir(&FS, "/iso/import"));
}

void test_scan_with_failed_directory_creation_returns_the_error(void) {
    mock_fs_free(&FS);
    mock_fs_init(&FS);
    IFS = mock_fs_iface(&FS);
    iso_catalog_init(&CAT, &IFS, &IRND);
    mock_fs_cut_at(&FS, 1, MOCK_TEAR_NONE); /* mkdir /iso の書き込みで電源断 */
    TEST_ASSERT_EQUAL_INT(ISO_ERR_IO, iso_catalog_scan(&CAT));
    TEST_ASSERT_FALSE(mock_fs_exists(&FS, "/iso"));
    /* /iso はあるが、/iso/import の作成で失敗 */
    mock_fs_power_on(&FS);
    TEST_ASSERT_EQUAL_INT(ISO_OK, mock_fs_add_dir(&FS, "/iso"));
    mock_fs_cut_at(&FS, 2, MOCK_TEAR_NONE); /* 1 回目 = mkdir /iso (EXISTS)、2 回目 = mkdir /iso/import */
    TEST_ASSERT_EQUAL_INT(ISO_ERR_IO, iso_catalog_scan(&CAT));
    TEST_ASSERT_FALSE(mock_fs_exists(&FS, "/iso/import"));
}

/* ---- received ---- */

static uint64_t received_for(const iso_meta_t *m, const char *id) {
    uint64_t r = 0xDEAD;
    TEST_ASSERT_EQUAL_INT(ISO_OK, iso_catalog_received(&IFS, id, m, &r));
    return r;
}

void test_received_counts_parts_in_order_and_stops_at_a_short_part(void) {
    iso_meta_t m = mk_meta("X", SIZE_25G, ISO_META_INCOMPLETE, 1);
    TEST_ASSERT_EQUAL_UINT16(3, m.part_count);
    TEST_ASSERT_EQUAL_UINT64(0, received_for(&m, "00000001")); /* ディレクトリも無い */
    TEST_ASSERT_EQUAL_INT(ISO_OK, mock_fs_add_dir(&FS, "/iso/00000001"));
    TEST_ASSERT_EQUAL_UINT64(0, received_for(&m, "00000001"));
    put_part("00000001", 0, 100);
    TEST_ASSERT_EQUAL_UINT64(100, received_for(&m, "00000001"));
    /* p000 が短いとき、p001 が満杯でも数えない(先頭から順、満たないパーツで止める) */
    put_part("00000001", 1, GIB);
    TEST_ASSERT_EQUAL_UINT64(100, received_for(&m, "00000001"));
}

void test_received_full_parts_and_last_short_part(void) {
    iso_meta_t m = mk_meta("X", SIZE_25G, ISO_META_INCOMPLETE, 1);
    put_slot("00000001", 0, &m);
    put_part("00000001", 0, GIB);
    TEST_ASSERT_EQUAL_UINT64(GIB, received_for(&m, "00000001"));
    put_part("00000001", 1, GIB);
    TEST_ASSERT_EQUAL_UINT64(2 * GIB, received_for(&m, "00000001"));
    put_part("00000001", 2, 512 * MIB - 1); /* 最後のパーツが 1 バイト足りない */
    TEST_ASSERT_EQUAL_UINT64(2 * GIB + 512 * MIB - 1, received_for(&m, "00000001"));
    TEST_ASSERT_EQUAL_INT(ISO_OK, IFS.remove(&FS, "/iso/00000001/p002"));
    put_part("00000001", 2, 512 * MIB); /* ちょうど */
    TEST_ASSERT_EQUAL_UINT64(SIZE_25G, received_for(&m, "00000001"));
}

void test_received_clamps_oversized_part_and_ignores_parts_after_a_gap(void) {
    iso_meta_t m = mk_meta("X", SIZE_25G, ISO_META_INCOMPLETE, 1);
    TEST_ASSERT_EQUAL_INT(ISO_OK, mock_fs_add_dir(&FS, "/iso/00000001"));
    put_part("00000001", 0, GIB + 4096); /* 期待より大きい(孤児のクラスタ) */
    TEST_ASSERT_EQUAL_UINT64(GIB, received_for(&m, "00000001"));
    put_part("00000001", 2, GIB); /* p001 が無い。p002 は数えない */
    TEST_ASSERT_EQUAL_UINT64(GIB, received_for(&m, "00000001"));
}

void test_received_does_not_look_at_parts_beyond_the_expected_count(void) {
    /* 1 GiB (既定のパーツ = 1 パーツ)だが、meta のパーツ数が 2。2 つ目の期待する大きさは 0 なので、p001 は調べない */
    iso_meta_t m = mk_meta("X", GIB, ISO_META_INCOMPLETE, 1);
    m.part_count = 2;
    TEST_ASSERT_EQUAL_INT(ISO_OK, mock_fs_add_dir(&FS, "/iso/00000001"));
    put_part("00000001", 0, GIB);
    mock_fs_fail_path(&FS, "/iso/00000001/p001");
    TEST_ASSERT_EQUAL_UINT64(GIB, received_for(&m, "00000001"));
    mock_fs_fail_path(&FS, NULL);
}

void test_received_part_that_is_a_directory_stops(void) {
    iso_meta_t m = mk_meta("X", SIZE_25G, ISO_META_INCOMPLETE, 1);
    TEST_ASSERT_EQUAL_INT(ISO_OK, mock_fs_add_dir(&FS, "/iso/00000001"));
    put_part("00000001", 0, GIB);
    TEST_ASSERT_EQUAL_INT(ISO_OK, mock_fs_add_dir(&FS, "/iso/00000001/p001"));
    TEST_ASSERT_EQUAL_UINT64(GIB, received_for(&m, "00000001"));
    TEST_ASSERT_TRUE(mock_fs_set_size(&FS, "/iso/00000001/p001", GIB)); /* 大きさが満杯でも、ディレクトリは数えない */
    TEST_ASSERT_EQUAL_UINT64(GIB, received_for(&m, "00000001"));
}

void test_received_follows_part_count_of_the_meta(void) {
    /* meta のパーツ数が 1 なら、p001 があっても数えない(meta が正) */
    iso_meta_t m = mk_meta("X", SIZE_25G, ISO_META_INCOMPLETE, 1);
    m.part_count = 1;
    TEST_ASSERT_EQUAL_INT(ISO_OK, mock_fs_add_dir(&FS, "/iso/00000001"));
    put_part("00000001", 0, GIB);
    put_part("00000001", 1, GIB);
    TEST_ASSERT_EQUAL_UINT64(GIB, received_for(&m, "00000001"));
}

void test_received_single_part_import_size(void) {
    /* import: 1 パーツ、part_blocks = ceil(size / 2048)。size = 5000 -> 3 ブロック。期待するパーツの大きさ = 5000 */
    iso_meta_t m = mk_meta("X", 5000, ISO_META_INVALID, 1);
    m.part_count = 1;
    m.part_blocks = 3;
    TEST_ASSERT_EQUAL_INT(ISO_OK, mock_fs_add_dir(&FS, "/iso/00000001"));
    put_part("00000001", 0, 5000);
    TEST_ASSERT_EQUAL_UINT64(5000, received_for(&m, "00000001"));
    TEST_ASSERT_EQUAL_INT(ISO_OK, IFS.remove(&FS, "/iso/00000001/p000"));
    put_part("00000001", 0, 5001); /* ブロックの端 (6144) までは許さない。期待する大きさで数える */
    TEST_ASSERT_EQUAL_UINT64(5000, received_for(&m, "00000001"));
}

void test_received_propagates_io_error(void) {
    iso_meta_t m = mk_meta("X", SIZE_25G, ISO_META_INCOMPLETE, 1);
    TEST_ASSERT_EQUAL_INT(ISO_OK, mock_fs_add_dir(&FS, "/iso/00000001"));
    put_part("00000001", 0, GIB);
    mock_fs_fail_path(&FS, "/iso/00000001/p001");
    uint64_t r = 0;
    TEST_ASSERT_EQUAL_INT(ISO_ERR_IO, iso_catalog_received(&IFS, "00000001", &m, &r));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_scan_empty_card_creates_directories);
    RUN_TEST(test_scan_ignores_directories_that_are_not_ids);
    RUN_TEST(test_scan_state_mapping_per_image);
    RUN_TEST(test_scan_resume_delete_only_for_deleting);
    RUN_TEST(test_scan_adopts_the_newer_slot);
    RUN_TEST(test_scan_limits_to_16_images_and_counts_hidden_in_directory_order);
    RUN_TEST(test_scan_exactly_16_images_has_no_hidden);
    RUN_TEST(test_scan_resets_previous_contents);
    RUN_TEST(test_scan_io_errors_are_propagated);
    RUN_TEST(test_find_returns_only_exact_ids);
    RUN_TEST(test_remove_entry_in_the_middle_and_at_the_end);
    RUN_TEST(test_ensure_dirs_reports_each_failure);
    RUN_TEST(test_scan_with_failed_directory_creation_returns_the_error);
    RUN_TEST(test_received_counts_parts_in_order_and_stops_at_a_short_part);
    RUN_TEST(test_received_full_parts_and_last_short_part);
    RUN_TEST(test_received_clamps_oversized_part_and_ignores_parts_after_a_gap);
    RUN_TEST(test_received_does_not_look_at_parts_beyond_the_expected_count);
    RUN_TEST(test_received_part_that_is_a_directory_stops);
    RUN_TEST(test_received_follows_part_count_of_the_meta);
    RUN_TEST(test_received_single_part_import_size);
    RUN_TEST(test_received_propagates_io_error);
    return UNITY_END();
}
